// The transport. See snps/transport.hpp for the contract.
//
// Two rules shape everything below:
//
//  1. Never call into libdatachannel while holding `mutex`. Its callbacks run
//     on its own threads and take `mutex` themselves, and some of its calls
//     can invoke callbacks before returning. Decide under the lock, act after
//     releasing it.
//  2. Never destroy a peer connection, or a WebSocket, from inside one of its
//     callbacks. A link that dies is moved to a graveyard, and a signalling
//     socket that closes to a list of dead ones; both are destroyed later from
//     a host thread or the ticker.
//
// The signalling connection comes back by itself once it has been welcomed:
// each loss schedules a reconnect with backoff, the hello carries the resume
// token from the last welcome so the server gives our peer id back, and the
// welcome is followed by our advert and our join, re-sent. Links do not
// depend on the server, and are kept throughout (docs/signalling-protocol.md,
// "Reconnecting").
//
// And one convention that avoids glare entirely: between any two peers, the
// one with the numerically lower id makes the offer. The other side, wanting
// to send first, asks for one with a "request" signal. Nobody ever has to
// resolve two crossing offers.
//
// Only API that libdatachannel and datachannel-wasm share is used here, which
// rules out negotiated channels, fixed channel ids, an explicit
// setLocalDescription, and every Configuration field but iceServers.

#include "snps/transport.hpp"

#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <utility>

namespace snps {

namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// Channel labels. The offerer creates both; the answerer tells them apart by
// label because the browser API cannot pre-negotiate channel ids.
constexpr const char* kUnreliableLabel = "snps-unreliable";
constexpr const char* kReliableLabel = "snps-reliable";

// The server drops a room its host has not refreshed for 30 seconds.
constexpr auto kAdvertInterval = 10s;
// A link that has not opened both channels by now is abandoned, and the next
// send starts over. That goes for a placeholder link too -- requests sent,
// no offer back -- however often it has asked, or a peer that keeps sending
// to someone who has gone would never hear from lost().
constexpr auto kConnectTimeout = 20s;
// How often a peer that must not offer re-asks the other side to.
constexpr auto kRequestInterval = 2s;
constexpr auto kTickInterval = 1s;
// Lost links waiting for lost(). A host that never asks -- storm has no use
// for it -- must not make this grow without bound.
constexpr std::size_t kLostLimit = 256;

// Reconnecting to the signalling server, once it has welcomed us: the first
// retry about a second after the loss, doubling to at most half a minute,
// every delay jittered, so that the clients one restart cut off together do
// not all come back together.
constexpr auto kReconnectFirst = 1s;
constexpr auto kReconnectMost = 30s;
// A connection that lasted this long resets the backoff. One cut sooner -- a
// server that welcomes and then dies -- keeps backing off.
constexpr auto kStableConnection = 10s;
// After a reconnect, `join` is re-sent until the server confirms it. The host
// may not have re-advertised yet: it is reconnecting too, on a schedule of its
// own that can run up to kReconnectMost behind ours.
constexpr auto kRejoinInterval = 3s;
constexpr auto kRejoinPatience = 60s;
#ifndef __EMSCRIPTEN__
// Natively, the WebSocket pings after this much silence from the server, and
// gives up after this many pings go unanswered. So a server that vanished
// without closing anything -- a machine that lost power, a NAT that forgot the
// connection -- is noticed within half a minute. A browser's WebSocket cannot
// ping, and notices only when TCP does.
constexpr auto kServerPingInterval = 10s;
constexpr int kServerPingsUnanswered = 2;
#endif

// ---- Logging ---------------------------------------------------------------

std::mutex g_logMutex;
std::function<void(LogLevel, const std::string&)> g_logSink;

void log(LogLevel level, const std::string& text) {
    std::function<void(LogLevel, const std::string&)> sink;
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        sink = g_logSink;
    }
    if (sink)
        sink(level, text);
}

void debug(const std::string& text) { log(LogLevel::Debug, text); }
void info(const std::string& text) { log(LogLevel::Info, text); }
void warn(const std::string& text) { log(LogLevel::Warning, text); }

std::string shortId(const PeerId& id) { return toHex(id).substr(0, 8); }

// ---- Encoding --------------------------------------------------------------

const char kBase64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const std::uint8_t* bytes, std::size_t size) {
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < size; i += 3) {
        const unsigned v = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
        out += kBase64[(v >> 18) & 63];
        out += kBase64[(v >> 12) & 63];
        out += kBase64[(v >> 6) & 63];
        out += kBase64[v & 63];
    }
    if (i + 1 == size) {
        const unsigned v = bytes[i] << 16;
        out += kBase64[(v >> 18) & 63];
        out += kBase64[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == size) {
        const unsigned v = (bytes[i] << 16) | (bytes[i + 1] << 8);
        out += kBase64[(v >> 18) & 63];
        out += kBase64[(v >> 12) & 63];
        out += kBase64[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

std::string base64Encode(const std::vector<std::uint8_t>& bytes) {
    return base64Encode(bytes.data(), bytes.size());
}

std::vector<std::uint8_t> base64Decode(std::string_view text) {
    std::vector<std::uint8_t> out;
    unsigned buffer = 0;
    int bits = 0;
    for (const char c : text) {
        const char* at = std::strchr(kBase64, c);
        if (c == '=' || !at || !c)
            break;
        buffer = (buffer << 6) | static_cast<unsigned>(at - kBase64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

std::string clamp(std::string text, std::size_t limit) {
    if (text.size() > limit)
        text.resize(limit);
    return text;
}

// Storm's strings are bytes in the game's code page, and some of them are
// structure -- StarCraft's stat string separates fields with CR. On the wire
// each byte is one character, U+0001 to U+00FF (docs/signalling-protocol.md):
// JSON carries that losslessly, where raw bytes above 0x7F would not be valid
// UTF-8 and nlohmann would refuse to serialise them. NUL cannot be in a C
// string anyway and is dropped.
std::string toWire(const std::string& bytes) {
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const char ch : bytes) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == 0)
            continue;
        if (c < 0x80) {
            out += ch;
        } else {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// The inverse. A character beyond U+00FF, from a client that does not follow
// the convention, has no byte to become and turns into '?'.
std::string fromWire(const std::string& utf8) {
    std::string out;
    out.reserve(utf8.size());
    for (std::size_t i = 0; i < utf8.size();) {
        const auto lead = static_cast<unsigned char>(utf8[i]);
        std::size_t length = 1;
        std::uint32_t code = lead;
        if (lead >= 0xF0) {
            length = 4;
            code = lead & 0x07;
        } else if (lead >= 0xE0) {
            length = 3;
            code = lead & 0x0F;
        } else if (lead >= 0xC0) {
            length = 2;
            code = lead & 0x1F;
        } else if (lead >= 0x80) {
            code = '?';  // a stray continuation byte
        }
        if (i + length > utf8.size())
            break;
        for (std::size_t k = 1; k < length; ++k)
            code = (code << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
        i += length;
        if (code != 0)
            out += code <= 0xFF ? static_cast<char>(code) : '?';
    }
    return out;
}

// `nonce` is picked once per game. The server derives the room id from it and
// our peer id, so the same game, re-advertised after a reconnect, is the same
// room -- on the same server or on its replacement.
json advertMessage(const Advert& advert, const std::string& nonce) {
    return {{"type", "advertise"},
            {"name", toWire(clamp(advert.name, 127))},
            {"stat", toWire(clamp(advert.stat, 127))},
            {"state", advert.state},
            {"max_players", advert.maxPlayers},
            {"extra", base64Encode(advert.extra)},
            {"nonce", nonce}};
}

json joinMessage(const std::string& game) { return {{"type", "join"}, {"game", game}}; }

std::string randomHex(std::mt19937_64& random, std::size_t bytes) {
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < bytes; ++i) {
        const auto byte = static_cast<unsigned>(random() & 0xFF);
        out += kDigits[byte >> 4];
        out += kDigits[byte & 15];
    }
    return out;
}

long long milliseconds(Clock::duration duration) {
    return static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
}

} // namespace

std::string toHex(const PeerId& id) {
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(id.size() * 2);
    for (const auto byte : id) {
        out += kDigits[byte >> 4];
        out += kDigits[byte & 15];
    }
    return out;
}

std::optional<PeerId> peerIdFromHex(std::string_view hex) {
    PeerId id{};
    if (hex.size() != id.size() * 2)
        return std::nullopt;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < id.size(); ++i) {
        const int hi = nibble(hex[2 * i]);
        const int lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return std::nullopt;
        id[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return id;
}

// ---- A peer and the connection to it ---------------------------------------

namespace {

struct Link {
    PeerId peer{};
    bool offerer = false;       // true when our id is the lower one
    bool pcPending = false;     // someone is building the connection right now
    Clock::time_point created = Clock::now();
    // Not a time_point{} for "never": that is only long ago where the clock
    // counts from boot. In a browser it counts from page load, and a request
    // made in the first two seconds of a page's life would never go out.
    std::optional<Clock::time_point> requested;

    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> unreliable;
    std::shared_ptr<rtc::DataChannel> reliable;
    bool unreliableOpen = false;
    bool reliableOpen = false;

    struct Held {
        Channel channel;
        rtc::binary data;
    };
    std::deque<Held> held;

    bool open() const { return unreliableOpen && reliableOpen; }
    std::shared_ptr<rtc::DataChannel>& channel(Channel c) {
        return c == Channel::Unreliable ? unreliable : reliable;
    }
    bool& isOpen(Channel c) {
        return c == Channel::Unreliable ? unreliableOpen : reliableOpen;
    }
};

} // namespace

// ---- The transport's state -------------------------------------------------

struct Transport::Impl : std::enable_shared_from_this<Transport::Impl> {
    explicit Impl(Options o) : options(std::move(o)) {}
    ~Impl() { stop(); }

    Options options;

    mutable std::mutex mutex;
    std::condition_variable changed;

    Status status = Status::Idle;
    std::string error;
    std::optional<PeerId> self;
    std::vector<rtc::IceServer> iceServers;
    std::shared_ptr<rtc::WebSocket> socket;

    // -- the signalling connection, and getting it back
    std::uint64_t generation = 0;  // the current socket's; older sockets' callbacks are ignored
    std::vector<std::shared_ptr<rtc::WebSocket>> deadSockets;  // closed, awaiting destruction
    bool everOnline = false;       // after the first welcome, a lost connection is retried
    std::string resumeToken;       // from the last welcome: our peer id, signed by the server
    std::optional<Clock::time_point> onlineSince;
    std::optional<Clock::time_point> reconnectAt;
    int failures = 0;              // connections lost or failed in a row, for the backoff
    std::mt19937_64 random{std::random_device{}()};

    std::optional<Advert> advert;
    std::string advertNonce;       // one per game: from advertise() to withdraw()
    Clock::time_point advertSent{};
    std::vector<Game> games;

    // The game join() was last asked for, joined again after a reconnect
    // until the server confirms it or kRejoinPatience runs out.
    std::optional<std::string> joined;
    std::optional<Clock::time_point> rejoinUntil;
    Clock::time_point rejoinSent{};

    std::map<PeerId, std::shared_ptr<Link>> links;
    std::vector<std::shared_ptr<Link>> graveyard;

    std::deque<Datagram> inbox;
    std::function<void()> receivable;
    std::uint64_t dropped = 0;
    std::deque<PeerId> lostPeers;

    std::optional<Clock::time_point> lastTick;  // optional for Link::requested's reason
    bool stopping = false;
    std::thread ticker;

    // -- lifecycle
    void start();
    void connect();
    void stop();
    void tick();
    void bury(std::vector<std::shared_ptr<Link>> dead);
    Clock::duration backoff();  // call with mutex held

    // -- signalling; `gen` is the socket's, from connect()
    void sendToServer(const json& message);
    void sendSignal(const PeerId& to, json data);
    void onServerMessage(std::uint64_t gen, const std::string& text);
    void onWelcome(std::uint64_t gen, const json& message);
    void onGames(const json& message);
    void onSignal(const PeerId& from, const json& data);
    void onServerClosed(std::uint64_t gen, const std::string& why);  // retries, once welcomed
    void giveUp(std::uint64_t gen, const std::string& why);          // never retries

    // -- peers
    std::shared_ptr<rtc::PeerConnection> makePeerConnection(const std::shared_ptr<Link>& link);
    void openAsOfferer(const std::shared_ptr<Link>& link);
    void acceptOffer(const PeerId& from, const std::string& sdp);
    void attach(const std::shared_ptr<Link>& link, const std::shared_ptr<rtc::DataChannel>& channel,
                Channel kind);
    void channelOpened(const std::shared_ptr<Link>& link, Channel kind);
    void linkFailed(const std::shared_ptr<Link>& link, const char* why);
    bool current(const std::shared_ptr<Link>& link) const;  // call with mutex held
    void noteLost(const PeerId& peer);                        // call with mutex held
    void deliver(const PeerId& from, Channel kind, const std::uint8_t* data, std::size_t size);
};

void Transport::Impl::start() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (status != Status::Idle)
            return;
        status = Status::Connecting;
    }
    connect();

#ifndef __EMSCRIPTEN__
    // Keeps the advert alive, reconnects and reaps dead links even when the
    // host is quiet. In a browser there is no thread to spare, and tick()
    // also runs opportunistically from every public call.
    std::weak_ptr<Impl> weak = shared_from_this();
    ticker = std::thread([weak] {
        for (;;) {
            auto impl = weak.lock();
            if (!impl)
                return;
            {
                std::unique_lock<std::mutex> lock(impl->mutex);
                impl->changed.wait_for(lock, kTickInterval, [&] { return impl->stopping; });
                if (impl->stopping)
                    return;
            }
            impl->tick();
        }
    });
#endif
}

// Opens a signalling connection: the first, or a replacement for one lost.
void Transport::Impl::connect() {
#ifndef __EMSCRIPTEN__
    rtc::WebSocketConfiguration config;
    config.pingInterval = kServerPingInterval;
    config.maxOutstandingPings = kServerPingsUnanswered;
    auto ws = std::make_shared<rtc::WebSocket>(config);
#else
    auto ws = std::make_shared<rtc::WebSocket>();
#endif
    std::uint64_t gen = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping)
            return;
        gen = ++generation;
        socket = ws;
        reconnectAt.reset();
    }

    std::weak_ptr<Impl> weak = shared_from_this();

    ws->onOpen([weak, gen] {
        auto impl = weak.lock();
        if (!impl)
            return;
        json hello = {{"type", "hello"},
                      {"product", impl->options.product},
                      {"version", impl->options.version},
                      {"name", impl->options.name.empty()
                                   ? std::string("player")
                                   : toWire(clamp(impl->options.name, 127))},
                      {"open", impl->options.open}};
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            if (impl->generation != gen)
                return;
            if (!impl->resumeToken.empty())
                hello["resume"] = impl->resumeToken;
        }
        info(hello.contains("resume") ? "signalling connected, asking for our peer id back"
                                      : "signalling connected, saying hello");
        impl->sendToServer(hello);
    });
    ws->onMessage([](rtc::binary) {},
                  [weak, gen](std::string text) {
                      if (auto impl = weak.lock())
                          impl->onServerMessage(gen, text);
                  });
    ws->onError([weak, gen](std::string what) {
        if (auto impl = weak.lock())
            impl->onServerClosed(gen, "signalling error: " + what);
    });
    ws->onClosed([weak, gen] {
        if (auto impl = weak.lock())
            impl->onServerClosed(gen, "signalling connection closed");
    });

    info("connecting to " + options.server);
    try {
        ws->open(options.server);
    } catch (const std::exception& e) {
        onServerClosed(gen, std::string("cannot open signalling connection: ") + e.what());
    }
}

void Transport::Impl::stop() {
    std::vector<std::shared_ptr<rtc::WebSocket>> sockets;
    std::vector<std::shared_ptr<Link>> dead;
    std::thread thread;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping)
            return;
        stopping = true;
        if (socket)
            sockets.push_back(std::move(socket));
        sockets.insert(sockets.end(), deadSockets.begin(), deadSockets.end());
        deadSockets.clear();
        for (auto& entry : links)
            dead.push_back(entry.second);
        links.clear();
        dead.insert(dead.end(), graveyard.begin(), graveyard.end());
        graveyard.clear();
        thread = std::move(ticker);
    }
    changed.notify_all();

    if (thread.joinable()) {
        if (thread.get_id() == std::this_thread::get_id())
            thread.detach();  // the ticker held the last reference
        else
            thread.join();
    }

    bury(std::move(dead));
    for (auto& ws : sockets) {
        try {
            ws->close();
        } catch (...) {
        }
    }
}

void Transport::Impl::bury(std::vector<std::shared_ptr<Link>> dead) {
    for (auto& link : dead) {
        if (link->pc) {
            try {
                link->pc->close();
            } catch (...) {
            }
        }
    }
    dead.clear();  // destroyed here, on a thread that is not one of theirs
}

// The delay before the next reconnect. Call with mutex held.
Clock::duration Transport::Impl::backoff() {
    Clock::duration delay = kReconnectFirst;
    for (int i = 0; i < failures && delay < kReconnectMost; ++i)
        delay *= 2;
    delay = std::min<Clock::duration>(delay, kReconnectMost);
    ++failures;
    std::uniform_int_distribution<Clock::rep> jitter(delay.count() / 2, delay.count());
    return Clock::duration(jitter(random));
}

void Transport::Impl::tick() {
    const auto now = Clock::now();
    std::optional<Advert> refresh;
    std::string nonce;
    std::optional<std::string> rejoin;
    bool reconnect = false;
    std::vector<std::shared_ptr<Link>> dead;
    std::vector<std::shared_ptr<rtc::WebSocket>> deadWs;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping || (lastTick && now - *lastTick < kTickInterval))
            return;
        lastTick = now;

        if (status == Status::Online && advert && now - advertSent >= kAdvertInterval) {
            refresh = advert;
            nonce = advertNonce;
            advertSent = now;
        }

        if (status == Status::Online && joined && rejoinUntil &&
            now - rejoinSent >= kRejoinInterval) {
            if (now >= *rejoinUntil) {
                warn("game " + *joined + " did not come back after the reconnect; giving up on it");
                joined.reset();
                rejoinUntil.reset();
            } else {
                rejoin = joined;
                rejoinSent = now;
            }
        }

        if (status == Status::Connecting && !socket && reconnectAt && now >= *reconnectAt) {
            reconnectAt.reset();
            reconnect = true;
        }

        for (auto it = links.begin(); it != links.end();) {
            const auto& link = it->second;
            const bool stalled = !link->open() && now - link->created > kConnectTimeout;
            if (stalled && !link->pcPending) {
                warn("no connection to " + shortId(link->peer) + " after " +
                     std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                        now - link->created)
                                        .count()) +
                     "s; will retry on the next send");
                dropped += link->held.size();
                noteLost(link->peer);
                dead.push_back(link);
                it = links.erase(it);
            } else {
                ++it;
            }
        }

        dead.insert(dead.end(), graveyard.begin(), graveyard.end());
        graveyard.clear();
        deadWs.swap(deadSockets);
    }

    if (reconnect)
        connect();
    if (refresh)
        sendToServer(advertMessage(*refresh, nonce));
    if (rejoin)
        sendToServer(joinMessage(*rejoin));
    bury(std::move(dead));
    for (auto& ws : deadWs) {
        try {
            ws->close();  // already closed, unless we gave up on it
        } catch (...) {
        }
    }
}

// ---- Signalling ------------------------------------------------------------

void Transport::Impl::sendToServer(const json& message) {
    std::shared_ptr<rtc::WebSocket> ws;
    {
        std::lock_guard<std::mutex> lock(mutex);
        ws = socket;
    }
    if (!ws)
        return;
    try {
        if (ws->isOpen())
            ws->send(message.dump());
    } catch (const std::exception& e) {
        warn(std::string("signalling send failed: ") + e.what());
    }
}

void Transport::Impl::sendSignal(const PeerId& to, json data) {
    sendToServer({{"type", "signal"}, {"to", toHex(to)}, {"data", std::move(data)}});
}

// The signalling connection failed or closed. Before the first welcome that is
// final, and the host decides what to do about it; after, it is retried.
void Transport::Impl::onServerClosed(std::uint64_t gen, const std::string& why) {
    std::optional<Clock::duration> retry;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping || gen != generation || !socket)
            return;  // stopping, or a socket already let go of
        deadSockets.push_back(std::move(socket));
        error = why;
        if (everOnline) {
            const auto now = Clock::now();
            if (onlineSince && now - *onlineSince >= kStableConnection)
                failures = 0;
            onlineSince.reset();
            status = Status::Connecting;
            retry = backoff();
            reconnectAt = now + *retry;
        } else {
            status = Status::Offline;
        }
    }
    warn(retry ? why + "; reconnecting in " + std::to_string(milliseconds(*retry)) + " ms" : why);
    changed.notify_all();
}

// Lets the signalling connection go for good: the server will not have us.
void Transport::Impl::giveUp(std::uint64_t gen, const std::string& why) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping || gen != generation || status == Status::Offline)
            return;
        if (socket)
            deadSockets.push_back(std::move(socket));  // closed by the next tick
        ++generation;  // and nothing more from it is heard
        status = Status::Offline;
        error = why;
        reconnectAt.reset();
    }
    warn(why);
    changed.notify_all();
}

void Transport::Impl::onServerMessage(std::uint64_t gen, const std::string& text) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (gen != generation)
            return;  // from a socket already let go of
    }
    json message;
    try {
        message = json::parse(text);
    } catch (const std::exception& e) {
        warn(std::string("unreadable signalling message: ") + e.what());
        return;
    }
    const std::string type = message.value("type", "");

    try {
        if (type == "welcome") {
            onWelcome(gen, message);
        } else if (type == "games") {
            onGames(message);
        } else if (type == "signal") {
            const auto from = peerIdFromHex(message.value("from", ""));
            if (from && message.contains("data") && message["data"].is_object())
                onSignal(*from, message["data"]);
        } else if (type == "advertised") {
            debug("advertised as game " + message.value("game", ""));
        } else if (type == "joined") {
            const std::string game = message.value("game", "");
            bool again = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                again = joined && *joined == game && rejoinUntil;
                if (again)
                    rejoinUntil.reset();
            }
            info((again ? "back in game " : "joined game ") + game);
        } else if (type == "game_closed") {
            const std::string id = message.value("game", "");
            std::lock_guard<std::mutex> lock(mutex);
            games.erase(std::remove_if(games.begin(), games.end(),
                                       [&](const Game& g) { return g.id == id; }),
                        games.end());
            if (joined && *joined == id) {
                joined.reset();  // nothing to join again
                rejoinUntil.reset();
            }
        } else if (type == "error") {
            const std::string code = message.value("code", "error");
            const std::string what = code + ": " + message.value("message", "");
            warn("signalling server says " + what);
            bool refused = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                refused = status == Status::Connecting;
            }
            // Before the welcome, an error means the hello was refused, and
            // would be again. After it, "replaced" means another connection
            // resumed our peer id: someone else holds our token, and taking it
            // back would only start a tug of war.
            if (refused)
                giveUp(gen, "signalling server refused us: " + what);
            else if (code == "replaced")
                giveUp(gen, "another connection took over our peer id");
        } else if (type == "relay" || type == "multicast") {
            // Out-of-band datagrams that came through the server instead of a
            // peer connection: DevilutionX's game-list query and its answers.
            const auto from = peerIdFromHex(message.value("from", ""));
            const auto data = base64Decode(message.value("data", ""));
            if (from && !data.empty())
                deliver(*from, Channel::Server, data.data(), data.size());
        } else if (type == "peer_joined" || type == "peer_left" || type == "broadcast") {
            debug("signalling: " + type);
        } else {
            debug("signalling: ignoring " + type);
        }
    } catch (const std::exception& e) {
        warn("malformed " + type + " message: " + e.what());
    }
}

void Transport::Impl::onWelcome(std::uint64_t gen, const json& message) {
    const auto id = peerIdFromHex(message.value("peer", ""));
    if (!id) {
        giveUp(gen, "signalling server sent no usable peer id");
        return;
    }

    std::vector<rtc::IceServer> servers;
    for (const auto& entry : message.value("ice", json::array())) {
        std::vector<std::string> urls;
        const auto& raw = entry.contains("urls") ? entry["urls"] : json();
        if (raw.is_string())
            urls.push_back(raw.get<std::string>());
        else if (raw.is_array())
            for (const auto& url : raw)
                urls.push_back(url.get<std::string>());

        for (const auto& url : urls) {
            try {
                // Parsed natively, passed through verbatim in the browser;
                // setting the credentials afterwards works for both.
                rtc::IceServer server(url);
                server.username = entry.value("username", "");
                server.password = entry.value("credential", "");
                servers.push_back(std::move(server));
            } catch (const std::exception& e) {
                warn("ignoring ICE server " + url + ": " + e.what());
            }
        }
    }

    // What the server forgot, or may have, goes back: our advert, and the
    // game we joined. Both are harmless to repeat.
    const auto now = Clock::now();
    std::optional<PeerId> previous;
    std::optional<Advert> pending;
    std::string nonce;
    std::optional<std::string> rejoin;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (gen != generation)
            return;
        previous = self;
        self = id;
        iceServers = std::move(servers);  // fresh TURN credentials, too
        status = Status::Online;
        error.clear();
        everOnline = true;
        onlineSince = now;
        resumeToken = message.value("resume", "");
        if (advert) {
            pending = advert;
            nonce = advertNonce;
            advertSent = now;
        }
        if (joined) {
            rejoin = joined;
            rejoinSent = now;
            rejoinUntil = now + kRejoinPatience;
        }
    }
    const std::string ice =
        std::to_string(message.value("ice", json::array()).size()) + " ICE server(s)";
    if (!previous)
        info("online as " + toHex(*id) + " with " + ice);
    else if (*previous == *id)
        info("back online as " + toHex(*id) + " with " + ice);
    else
        warn("back online, but as " + toHex(*id) + " rather than " + toHex(*previous) +
             ": the server could not honour our resume token, so a peer that knows the old "
             "id cannot reach us through it");
    changed.notify_all();

    if (pending)
        sendToServer(advertMessage(*pending, nonce));
    if (rejoin)
        sendToServer(joinMessage(*rejoin));
}

void Transport::Impl::onGames(const json& message) {
    std::vector<Game> fresh;
    for (const auto& entry : message.value("games", json::array())) {
        const auto host = peerIdFromHex(entry.value("host", ""));
        if (!host)
            continue;
        Game game;
        game.id = entry.value("game", "");
        game.name = fromWire(entry.value("name", ""));
        game.stat = fromWire(entry.value("stat", ""));
        game.state = entry.value("state", 0u);
        game.host = *host;
        game.players = entry.value("players", 0);
        game.maxPlayers = entry.value("max_players", 0);
        game.extra = base64Decode(entry.value("extra", ""));
        fresh.push_back(std::move(game));
    }
    std::lock_guard<std::mutex> lock(mutex);
    games = std::move(fresh);
}

void Transport::Impl::onSignal(const PeerId& from, const json& data) {
    const std::string kind = data.value("kind", "");

    if (kind == "request") {
        std::shared_ptr<Link> link;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!self || !(*self < from))
                return;  // the rule says they offer, not us
            auto& slot = links[from];
            if (slot && slot->open() && !slot->pcPending) {
                // Asked for a connection we think is open: theirs is gone,
                // disconnect()ed or lost on their side only, so ours is stale.
                // Start over, as acceptOffer does in the other role.
                info(shortId(from) + " asked to connect again; starting over");
                auto fresh = std::make_shared<Link>();
                fresh->peer = from;
                fresh->offerer = true;
                fresh->held = std::move(slot->held);
                graveyard.push_back(slot);
                slot = fresh;
            }
            if (!slot) {
                slot = std::make_shared<Link>();
                slot->peer = from;
                slot->offerer = true;
            }
            if (slot->pc || slot->pcPending)
                return;  // already on it
            slot->pcPending = true;
            slot->created = Clock::now();
            link = slot;
        }
        debug(shortId(from) + " asked for an offer");
        openAsOfferer(link);
        return;
    }

    if (kind == "offer") {
        acceptOffer(from, data.value("sdp", ""));
        return;
    }

    std::shared_ptr<rtc::PeerConnection> pc;
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = links.find(from);
        if (it != links.end())
            pc = it->second->pc;
    }
    if (!pc) {
        debug("signal '" + kind + "' from " + shortId(from) + " with no connection; ignored");
        return;
    }

    try {
        if (kind == "answer") {
            pc->setRemoteDescription(rtc::Description(data.value("sdp", ""), "answer"));
        } else if (kind == "candidate") {
            pc->addRemoteCandidate(
                rtc::Candidate(data.value("candidate", ""), data.value("mid", "")));
        }
    } catch (const std::exception& e) {
        warn("applying " + kind + " from " + shortId(from) + ": " + e.what());
    }
}

// ---- Peers -----------------------------------------------------------------

bool Transport::Impl::current(const std::shared_ptr<Link>& link) const {
    const auto it = links.find(link->peer);
    return it != links.end() && it->second == link;
}

void Transport::Impl::noteLost(const PeerId& peer) {
    if (std::find(lostPeers.begin(), lostPeers.end(), peer) != lostPeers.end())
        return;
    if (lostPeers.size() >= kLostLimit)
        lostPeers.pop_front();
    lostPeers.push_back(peer);
}

std::shared_ptr<rtc::PeerConnection>
Transport::Impl::makePeerConnection(const std::shared_ptr<Link>& link) {
    rtc::Configuration config;
    {
        std::lock_guard<std::mutex> lock(mutex);
        config.iceServers = iceServers;
    }
#ifndef __EMSCRIPTEN__
    if (options.relayOnly)
        config.iceTransportPolicy = rtc::TransportPolicy::Relay;
#endif

    auto pc = std::make_shared<rtc::PeerConnection>(config);
    std::weak_ptr<Impl> weakImpl = shared_from_this();
    std::weak_ptr<Link> weakLink = link;
    const PeerId peer = link->peer;

    pc->onLocalDescription([weakImpl, peer](rtc::Description description) {
        if (auto impl = weakImpl.lock())
            impl->sendSignal(peer, {{"kind", description.typeString()},
                                    {"sdp", std::string(description)}});
    });
    pc->onLocalCandidate([weakImpl, peer](rtc::Candidate candidate) {
        if (auto impl = weakImpl.lock())
            impl->sendSignal(peer, {{"kind", "candidate"},
                                    {"candidate", std::string(candidate)},
                                    {"mid", candidate.mid()}});
    });
    pc->onStateChange([weakImpl, weakLink](rtc::PeerConnection::State state) {
        auto impl = weakImpl.lock();
        auto link = weakLink.lock();
        if (!impl || !link)
            return;
        switch (state) {
        case rtc::PeerConnection::State::Connected:
            info("connected to " + shortId(link->peer));
            break;
        case rtc::PeerConnection::State::Disconnected:
            info("connection to " + shortId(link->peer) + " interrupted");
            break;
        case rtc::PeerConnection::State::Failed:
            impl->linkFailed(link, "failed");
            break;
        case rtc::PeerConnection::State::Closed:
            impl->linkFailed(link, "closed");
            break;
        default:
            break;
        }
    });
    pc->onDataChannel([weakImpl, weakLink](std::shared_ptr<rtc::DataChannel> channel) {
        auto impl = weakImpl.lock();
        auto link = weakLink.lock();
        if (!impl || !link)
            return;
        const std::string label = channel->label();
        if (label == kUnreliableLabel)
            impl->attach(link, channel, Channel::Unreliable);
        else if (label == kReliableLabel)
            impl->attach(link, channel, Channel::Reliable);
        else
            warn("unexpected channel '" + label + "' from " + shortId(link->peer));
    });
    return pc;
}

void Transport::Impl::openAsOfferer(const std::shared_ptr<Link>& link) {
    try {
        auto pc = makePeerConnection(link);

        rtc::DataChannelInit unreliable;
        unreliable.reliability.unordered = true;
        unreliable.reliability.maxRetransmits = 0;

        bool abandoned = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            link->pc = pc;
            // disconnect() or stop() took the link away while this was built.
            abandoned = !current(link);
        }
        if (abandoned) {
            pc->close();
        } else {
            // Creating the first channel is what starts negotiation.
            auto u = pc->createDataChannel(kUnreliableLabel, unreliable);
            auto r = pc->createDataChannel(kReliableLabel);
            attach(link, u, Channel::Unreliable);
            attach(link, r, Channel::Reliable);
            debug("offering to " + shortId(link->peer));
        }
    } catch (const std::exception& e) {
        warn("cannot connect to " + shortId(link->peer) + ": " + e.what());
        linkFailed(link, "could not be created");
    }
    std::lock_guard<std::mutex> lock(mutex);
    link->pcPending = false;
}

void Transport::Impl::acceptOffer(const PeerId& from, const std::string& sdp) {
    std::shared_ptr<Link> link;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!self || !(from < *self)) {
            warn("offer from " + shortId(from) + ", who should be answering us; ignored");
            return;
        }
        auto& slot = links[from];
        if (slot && (slot->pc || slot->pcPending)) {
            // The other side started over -- its old connection failed, or it
            // restarted. Replace ours rather than confuse the two.
            auto fresh = std::make_shared<Link>();
            fresh->peer = from;
            fresh->held = std::move(slot->held);
            graveyard.push_back(slot);
            slot = fresh;
        } else if (!slot) {
            slot = std::make_shared<Link>();
            slot->peer = from;
        }
        slot->offerer = false;
        slot->pcPending = true;
        slot->created = Clock::now();
        link = slot;
    }

    debug("answering " + shortId(from));
    try {
        auto pc = makePeerConnection(link);
        bool abandoned = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            link->pc = pc;
            abandoned = !current(link);  // as in openAsOfferer
        }
        if (abandoned)
            pc->close();
        else
            pc->setRemoteDescription(rtc::Description(sdp, "offer"));
    } catch (const std::exception& e) {
        warn("cannot answer " + shortId(from) + ": " + e.what());
        linkFailed(link, "could not be answered");
    }
    std::lock_guard<std::mutex> lock(mutex);
    link->pcPending = false;
}

void Transport::Impl::attach(const std::shared_ptr<Link>& link,
                             const std::shared_ptr<rtc::DataChannel>& channel, Channel kind) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        link->channel(kind) = channel;
    }

    std::weak_ptr<Impl> weakImpl = shared_from_this();
    std::weak_ptr<Link> weakLink = link;
    const PeerId peer = link->peer;

    channel->onOpen([weakImpl, weakLink, kind] {
        auto impl = weakImpl.lock();
        auto link = weakLink.lock();
        if (impl && link)
            impl->channelOpened(link, kind);
    });
    channel->onClosed([weakImpl, weakLink, kind] {
        auto impl = weakImpl.lock();
        auto link = weakLink.lock();
        if (!impl || !link)
            return;
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            link->isOpen(kind) = false;
        }
        // Nothing on this side closes a channel of a link that is still
        // current, so the other side did: it disconnected, or went away.
        // Saying so now beats waiting for ICE to time out.
        impl->linkFailed(link, "closed by the other side");
    });
    channel->onMessage(
        [weakImpl, peer, kind](rtc::binary data) {
            if (auto impl = weakImpl.lock())
                impl->deliver(peer, kind, reinterpret_cast<const std::uint8_t*>(data.data()),
                              data.size());
        },
        [](std::string) {});

    // A channel the other side opened arrives already open, and its onOpen
    // may never fire.
    if (channel->isOpen())
        channelOpened(link, kind);
}

void Transport::Impl::channelOpened(const std::shared_ptr<Link>& link, Channel kind) {
    // Drain what was held, in order, before letting send() go direct -- the
    // reliable channel promises ordering, and a datagram sent straight past
    // the held ones would break it.
    for (;;) {
        std::deque<Link::Held> batch;
        std::shared_ptr<rtc::DataChannel> channel;
        {
            std::lock_guard<std::mutex> lock(mutex);
            channel = link->channel(kind);
            if (!channel)
                return;
            for (auto it = link->held.begin(); it != link->held.end();) {
                if (it->channel == kind) {
                    batch.push_back(std::move(*it));
                    it = link->held.erase(it);
                } else {
                    ++it;
                }
            }
            if (batch.empty()) {
                const bool first = !link->isOpen(kind);
                link->isOpen(kind) = true;
                if (first && link->open())
                    debug("both channels to " + shortId(link->peer) + " open");
                return;
            }
        }
        for (auto& item : batch) {
            try {
                channel->send(std::move(item.data));
            } catch (const std::exception& e) {
                warn("flushing to " + shortId(link->peer) + ": " + e.what());
            }
        }
    }
}

void Transport::Impl::linkFailed(const std::shared_ptr<Link>& link, const char* why) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        // A link that is no longer current was disconnect()ed, replaced, or
        // let go by stop(), and none of those is a loss worth reporting.
        if (!current(link))
            return;
        dropped += link->held.size();
        link->held.clear();
        link->unreliableOpen = false;
        link->reliableOpen = false;
        graveyard.push_back(link);
        links.erase(link->peer);
        noteLost(link->peer);
    }
    info("connection to " + shortId(link->peer) + " " + why);
}

void Transport::Impl::deliver(const PeerId& from, Channel kind, const std::uint8_t* data,
                              std::size_t size) {
    std::function<void()> wake;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping)
            return;
        while (inbox.size() >= std::max<std::size_t>(options.inboxLimit, 1)) {
            inbox.pop_front();
            ++dropped;
        }
        Datagram datagram;
        datagram.from = from;
        datagram.channel = kind;
        datagram.data.assign(data, data + size);
        inbox.push_back(std::move(datagram));
        wake = receivable;
    }
    if (wake)
        wake();
}

// ---- The public face -------------------------------------------------------

Transport::Transport(Options options) : impl_(std::make_shared<Impl>(std::move(options))) {}

Transport::~Transport() {
    if (impl_)
        impl_->stop();
}

void Transport::start() { impl_->start(); }

Status Transport::status() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->status;
}

std::string Transport::error() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->error;
}

bool Transport::waitOnline(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->changed.wait_for(lock, timeout, [&] {
        return impl_->status == Status::Online || impl_->status == Status::Offline ||
               impl_->stopping;
    });
    return impl_->status == Status::Online;
}

std::optional<PeerId> Transport::self() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->self;
}

void Transport::advertise(const Advert& advert) {
    impl_->tick();
    bool online = false;
    std::string nonce;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        // A new game gets a nonce of its own; a changed advert is the same game.
        if (!impl_->advert)
            impl_->advertNonce = randomHex(impl_->random, 8);
        impl_->advert = advert;
        nonce = impl_->advertNonce;
        online = impl_->status == Status::Online;
        if (online)
            impl_->advertSent = Clock::now();
    }
    if (online)
        impl_->sendToServer(advertMessage(advert, nonce));
}

void Transport::withdraw() {
    bool had = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        had = impl_->advert.has_value();
        impl_->advert.reset();
        impl_->advertNonce.clear();
    }
    if (had)
        impl_->sendToServer({{"type", "unadvertise"}});
}

void Transport::refreshGames() {
    impl_->tick();
    impl_->sendToServer({{"type", "list"}});
}

std::vector<Game> Transport::games() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->games;
}

void Transport::join(const std::string& gameId) {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->joined = gameId;
        impl_->rejoinUntil.reset();
    }
    impl_->sendToServer(joinMessage(gameId));
}

bool Transport::send(const PeerId& to, Channel channel, const std::uint8_t* data,
                     std::size_t size) {
    if (!data || size == 0 || channel == Channel::Server)
        return false;
    impl_->tick();

    std::shared_ptr<Link> link;
    std::shared_ptr<rtc::DataChannel> direct;
    bool offer = false;
    bool request = false;
    bool loopback = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->self)
            return false;  // never online, so there is no link to use
        const PeerId& self = *impl_->self;
        const auto existing = impl_->links.find(to);

        if (to == self) {
            loopback = true;
        } else if (existing != impl_->links.end() && existing->second->isOpen(channel)) {
            // An open channel needs no server, so a game in progress survives
            // the signalling connection going down.
            direct = existing->second->channel(channel);
        } else if (impl_->status != Status::Online) {
            return false;  // but connecting anyone does need it
        } else {
            auto& slot = impl_->links[to];
            if (!slot) {
                slot = std::make_shared<Link>();
                slot->peer = to;
                slot->offerer = self < to;
            }
            link = slot;

            while (link->held.size() >= std::max<std::size_t>(impl_->options.heldLimit, 1)) {
                link->held.pop_front();
                ++impl_->dropped;
            }
            rtc::binary bytes(size);
            std::memcpy(bytes.data(), data, size);
            link->held.push_back({channel, std::move(bytes)});

            if (!link->pc && !link->pcPending) {
                const auto now = Clock::now();
                if (link->offerer) {
                    link->pcPending = true;
                    link->created = now;
                    offer = true;
                } else if (!link->requested || now - *link->requested >= kRequestInterval) {
                    if (!link->requested)
                        link->created = now;
                    link->requested = now;
                    request = true;
                }
            }
        }
    }

    if (loopback) {
        impl_->deliver(to, channel, data, size);
        return true;
    }
    if (direct) {
        try {
            direct->send(reinterpret_cast<const std::byte*>(data), size);
        } catch (const std::exception& e) {
            warn("send to " + shortId(to) + ": " + e.what());
        }
        return true;
    }
    if (offer)
        impl_->openAsOfferer(link);
    if (request)
        impl_->sendSignal(to, {{"kind", "request"}});
    return true;
}

bool Transport::receive(Datagram& out) {
    impl_->tick();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->inbox.empty())
        return false;
    out = std::move(impl_->inbox.front());
    impl_->inbox.pop_front();
    return true;
}

std::size_t Transport::pending() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->inbox.size();
}

bool Transport::sendViaServer(const PeerId& to, const std::uint8_t* data, std::size_t size) {
    if (!data || size == 0 || size > kMaxServerDatagram)
        return false;
    impl_->tick();
    bool loopback = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->status != Status::Online || !impl_->self)
            return false;
        loopback = to == *impl_->self;
    }
    if (loopback) {
        impl_->deliver(to, Channel::Server, data, size);
        return true;
    }
    impl_->sendToServer(
        {{"type", "relay"}, {"to", toHex(to)}, {"data", base64Encode(data, size)}});
    return true;
}

bool Transport::multicast(const std::uint8_t* data, std::size_t size) {
    if (!data || size == 0 || size > kMaxServerDatagram)
        return false;
    impl_->tick();
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->status != Status::Online)
            return false;
    }
    impl_->sendToServer({{"type", "multicast"}, {"data", base64Encode(data, size)}});
    return true;
}

void Transport::disconnect(const PeerId& peer) {
    std::shared_ptr<Link> link;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto it = impl_->links.find(peer);
        if (it == impl_->links.end())
            return;
        link = std::move(it->second);
        impl_->links.erase(it);
        // Dropped on purpose, so not counted by dropped().
        link->held.clear();
        link->unreliableOpen = false;
        link->reliableOpen = false;
    }
    info("disconnected from " + shortId(peer));
    // Closed from the caller's thread, which is not one of libdatachannel's.
    // Its state callbacks find the link no longer current, so lost() is quiet.
    impl_->bury({link});
}

bool Transport::lost(PeerId& out) {
    impl_->tick();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->lostPeers.empty())
        return false;
    out = impl_->lostPeers.front();
    impl_->lostPeers.pop_front();
    return true;
}

void Transport::onReceivable(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->receivable = std::move(callback);
}

bool Transport::connected(const PeerId& peer) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->links.find(peer);
    return it != impl_->links.end() && it->second->open();
}

std::size_t Transport::linkCount() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->links.size();
}

std::uint64_t Transport::dropped() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->dropped;
}

// ---- Process-wide ----------------------------------------------------------

namespace {
std::mutex g_initMutex;
int g_initCount = 0;
} // namespace

void initialize() {
    std::lock_guard<std::mutex> lock(g_initMutex);
    if (g_initCount++ > 0)
        return;
    rtc::InitLogger(rtc::LogLevel::Warning, [](rtc::LogLevel level, std::string message) {
        log(level <= rtc::LogLevel::Error ? LogLevel::Error : LogLevel::Warning,
            "rtc: " + message);
    });
    rtc::Preload();
}

void shutdown() {
    std::lock_guard<std::mutex> lock(g_initMutex);
    if (g_initCount == 0 || --g_initCount > 0)
        return;
    rtc::Cleanup().wait();
}

void setLogSink(std::function<void(LogLevel, const std::string&)> sink) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logSink = std::move(sink);
}

} // namespace snps
