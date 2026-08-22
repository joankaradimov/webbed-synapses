#ifndef SNPS_TRANSPORT_HPP
#define SNPS_TRANSPORT_HPP

// The transport: a signalling connection, a game directory, and one WebRTC
// peer connection per peer carrying two data channels.
//
// Written for two hosts that want different things (AGENT.md section 4):
// storm wants unreliable, unordered datagrams and does its own sequencing;
// DevilutionX wants a reliable, ordered stream per peer. Both get what they
// want from the same connection.
//
// Rules this lives under (core/README.md):
//  - Nothing here does anything at static-init time. Construction is cheap;
//    start() is where sockets and threads begin, and snps::initialize() must
//    have run first.
//  - The host pulls. receive() drains a queue; onReceivable() is a wake-up,
//    not a delivery, and is the only thing called from an internal thread.
//  - send() never blocks. A peer nobody has connected to yet is connected on
//    demand -- storm gossips peer ids and then sends to strangers
//    (docs/snp-abi.md section 6) -- and the datagram waits for the channel.
//  - No libdatachannel type appears in this header, so hosts compile against
//    it without the WebRTC headers, and the implementation stays inside the
//    API that libdatachannel and datachannel-wasm share.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace snps {

// A peer's identity: 128 bits minted by the signalling server, stable for the
// session and the same for every participant. Deliberately the size of
// storm's SNETADDR and of DevilutionX's endpoint::addr, so it passes through
// both adapters as opaque bytes. It survives a reconnect, to the same server
// or to its replacement, as long as that server shares the secret that signed
// it (docs/signalling-protocol.md, "Reconnecting").
using PeerId = std::array<std::uint8_t, 16>;

std::string toHex(const PeerId& id);
std::optional<PeerId> peerIdFromHex(std::string_view hex);

enum class Channel : std::uint8_t {
    Unreliable,  // unordered, never retransmitted -- storm's traffic
    Reliable,    // ordered, retransmitted -- DevilutionX's send()
    Server,      // received only: came through the signalling server, from
                 // sendViaServer() or multicast(), not over a peer connection
};

// The most sendViaServer() and multicast() carry. The server refuses more: it
// is there for small out-of-band datagrams, like a game-list query and its
// answers, and not to carry a game.
constexpr std::size_t kMaxServerDatagram = 1024;

struct Options {
    std::string server;         // ws://host:port of the signalling server
    std::string product;        // four characters, e.g. "SEXP"; scopes the directory
    int         version = 0;    // scopes the directory too
    std::string name;           // player name, informational

    // Inbound datagrams kept before the oldest are dropped. The host is
    // expected to drain promptly; this only bounds a host that does not.
    std::size_t inboxLimit = 4096;

    // Outbound datagrams held per peer while its connection is being set up.
    // Storm retransmits, so a short queue that bridges the handshake is all
    // that is useful; more would replay stale traffic.
    std::size_t heldLimit = 64;

    // Native builds only: route everything through TURN. For the harness,
    // which otherwise cannot exercise the relayed path on purpose. Ignored in
    // the browser, whose API has no equivalent.
    bool relayOnly = false;

    // Lets any peer with the same product and version reach this one --
    // connect to it, sendViaServer() to it, include it in multicast() --
    // without sharing a game. The exposure of a public LAN. DevilutionX's
    // peers are open, since its players find each other by multicast and
    // meet with no directory; storm's are not (docs/signalling-protocol.md).
    bool open = false;
};

// What a host publishes. The transport keeps it alive on the server until
// withdraw(), across reconnects; calling advertise() again replaces it, and
// it stays the same game, under the same id.
struct Advert {
    std::string               name;        // <= 127 bytes, storm's char[128]
    std::string               stat;        // <= 127 bytes
    std::uint32_t             state = 0;
    int                       maxPlayers = 8;
    std::vector<std::uint8_t> extra;       // <= 32 bytes
};

struct Game {
    std::string               id;
    std::string               name;
    std::string               stat;
    std::uint32_t             state = 0;
    PeerId                    host{};
    int                       players = 0;
    int                       maxPlayers = 0;
    std::vector<std::uint8_t> extra;
};

struct Datagram {
    PeerId                    from{};
    Channel                   channel = Channel::Unreliable;
    std::vector<std::uint8_t> data;
};

enum class Status {
    Idle,        // not started
    Connecting,  // waiting for the server's welcome: the first time, or again
                 // after the connection was lost, which the transport retries
                 // by itself. Links already open keep working meanwhile, and
                 // self() keeps the id it had.
    Online,      // welcomed; has a peer id
    Offline,     // given up; see error(). The first connection failed, or the
                 // server refused this transport, and nothing will retry.
                 // Links already open keep working; nothing new can start.
};

class Transport {
public:
    explicit Transport(Options options);
    ~Transport();

    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    // Opens the signalling connection. Returns at once. If that first
    // connection fails, the transport goes Offline and the host decides what
    // next. Once it has been Online, a lost connection is reopened with
    // backoff, and the server is given back what it may have forgotten: our
    // peer id, our advert and the game we joined. In a browser the retries
    // ride on the host's calls, like everything else the ticker does natively.
    void start();

    Status status() const;
    std::string error() const;

    // For hosts that have to answer synchronously whether the network works,
    // as storm's Initialize does. Returns true once Online, false on Offline
    // or timeout. Not for use from a browser's main thread.
    bool waitOnline(std::chrono::milliseconds timeout);

    // This peer's id, once Online. A reconnect keeps it, unless the server
    // cannot honour the resume token -- a server with another secret -- and
    // issues a new one, which is logged.
    std::optional<PeerId> self() const;

    // --- Directory -------------------------------------------------------

    void advertise(const Advert& advert);
    void withdraw();

    // Asks the server for the game list; the answer replaces games() when it
    // arrives. Cheap to call often -- storm polls about once a second.
    void refreshGames();
    std::vector<Game> games() const;

    // Joins a listed game, which is what makes signalling to its members
    // permitted. Traffic to the host may be sent immediately afterwards. The
    // game is remembered and joined again after a reconnect, until the server
    // says it closed.
    void join(const std::string& gameId);

    // --- Traffic -----------------------------------------------------------

    // Queues a datagram for a peer. Never blocks. A channel that is already
    // open needs no server, so it keeps working when the signalling connection
    // is lost; a peer not yet connected needs one. Returns false if the
    // datagram is empty, if `channel` is Server, or if the channel to that
    // peer is not open and the transport is not Online.
    bool send(const PeerId& to, Channel channel, const std::uint8_t* data,
              std::size_t size);

    // One datagram to one peer through the signalling server; it arrives as
    // Channel::Server and never opens a peer connection. The server delivers
    // it only to an open peer or to a member of our game. Returns false if
    // not Online, or if the datagram is empty or over kMaxServerDatagram.
    // Nothing reports whether it arrived.
    bool sendViaServer(const PeerId& to, const std::uint8_t* data, std::size_t size);

    // One datagram through the signalling server to every other open peer
    // with our product and version, as Channel::Server. Open or not, the
    // sender does not get a copy. The server allows a handful a second. Same
    // failures as sendViaServer().
    bool multicast(const std::uint8_t* data, std::size_t size);

    // Takes the oldest queued datagram. Returns false if there is none.
    bool receive(Datagram& out);
    std::size_t pending() const;

    // --- Links -------------------------------------------------------------

    // Closes the link to a peer and forgets it, with anything still waiting
    // for it. Not reported by lost(). The next send() to it starts over.
    void disconnect(const PeerId& peer);

    // Takes the next peer whose link was lost on its own: it failed or was
    // closed from the other side, or it never opened and was given up on.
    // disconnect(), a replacement link, and the transport's own destruction
    // are not reported. Returns false if there is none.
    bool lost(PeerId& out);

    // Called from an internal thread whenever a datagram is queued. Keep it to
    // SetEvent, a condition variable, or similar; never call back into the
    // transport from it.
    void onReceivable(std::function<void()> callback);

    // --- Introspection, for the harness and for diagnostics ----------------

    // True when both channels to this peer are open.
    bool connected(const PeerId& peer) const;
    std::size_t linkCount() const;

    // Datagrams dropped because the inbox was full or a held queue overflowed.
    std::uint64_t dropped() const;

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

// Process-wide setup and teardown for the WebRTC stack. initialize() starts
// nothing that outlives shutdown(); shutdown() waits for the stack's threads.
// Storm loads and unloads a module repeatedly, so both must be safe to call
// more than once over a process's life -- and never from DllMain.
void initialize();
void shutdown();

// Where the transport's diagnostics go. Unset, they are dropped. The sink is
// called from whichever thread logged, so it must be thread-safe.
enum class LogLevel { Debug, Info, Warning, Error };
void setLogSink(std::function<void(LogLevel, const std::string&)> sink);

} // namespace snps

#endif // SNPS_TRANSPORT_HPP
