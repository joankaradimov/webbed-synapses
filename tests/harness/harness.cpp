// Peers, no game.
//
// Drives real transports through a real signalling server and checks what
// both adapters are going to rely on. Run it through run.mjs, which starts the
// server; or point it at one that is already running:
//
//     snps-harness --server ws://127.0.0.1:8765 [--verbose]
//
// What it does not cover: the relayed path, which needs a TURN server, and
// peer loss detected by ICE timeouts, which takes longer than a test should.
//
// Its last checks kill the signalling server, when run.mjs says which process
// that is (SNPS_SERVER_PID), to see open links outlive it; and with run.mjs's
// --respawn, to see the transports come back to the server that replaces it.

#include "snps/transport.hpp"
#include "snps/version.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#endif

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    std::fflush(stdout);
    if (!ok)
        ++g_failures;
}

bool waitFor(const std::function<bool()>& done, std::chrono::milliseconds timeout) {
    const auto until = Clock::now() + timeout;
    while (Clock::now() < until) {
        if (done())
            return true;
        std::this_thread::sleep_for(20ms);
    }
    return done();
}

std::vector<std::uint8_t> bytes(const std::string& text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::string text(const std::vector<std::uint8_t>& data) {
    return std::string(data.begin(), data.end());
}

// A transport plus everything it has received so far.
struct Peer {
    std::string label;
    std::unique_ptr<snps::Transport> transport;
    std::vector<snps::Datagram> inbox;
    std::atomic<int> wakeups{0};

    // StarCraft's product unless it is open, or says otherwise: open peers are
    // DevilutionX's.
    Peer(std::string name, const std::string& server, bool open = false,
         bool devilution = false)
        : label(std::move(name)) {
        devilution = devilution || open;
        snps::Options options;
        options.server = server;
        options.product = devilution ? "DVLX" : "SEXP";
        options.version = devilution ? 1 : 211;
        options.name = label;
        options.open = open;
        transport = std::make_unique<snps::Transport>(options);
        transport->onReceivable([this] { ++wakeups; });
    }

    snps::PeerId id() const { return *transport->self(); }

    void collect() {
        snps::Datagram datagram;
        while (transport->receive(datagram))
            inbox.push_back(std::move(datagram));
    }

    bool send(const Peer& to, snps::Channel channel, const std::string& payload) {
        return transport->send(to.id(), channel, bytes(payload).data(), payload.size());
    }

    // Whether a datagram matching `payload` from `from` is in, on any channel
    // unless one is named.
    bool has(const Peer& from, const std::string& payload,
             const snps::Channel* channel = nullptr) {
        collect();
        for (const auto& d : inbox)
            if (d.from == from.id() && text(d.data) == payload &&
                (!channel || d.channel == *channel))
                return true;
        return false;
    }

    // Waits until it is.
    bool received(const Peer& from, const std::string& payload,
                  std::chrono::milliseconds timeout = 15s,
                  const snps::Channel* channel = nullptr) {
        return waitFor([&] { return has(from, payload, channel); }, timeout);
    }
};

const snps::Channel kServer = snps::Channel::Server;

bool online(std::initializer_list<Peer*> peers) {
    bool all = true;
    for (Peer* peer : peers)
        peer->transport->start();
    for (Peer* peer : peers)
        all = peer->transport->waitOnline(5s) && all;
    return all;
}

bool status(Peer& peer, snps::Status wanted) { return peer.transport->status() == wanted; }

// Asks the server for the game list until a game in it matches.
bool findGame(Peer& lister, const std::function<bool(const snps::Game&)>& match,
              snps::Game& found, std::chrono::milliseconds timeout) {
    return waitFor(
        [&] {
            lister.transport->refreshGames();
            std::this_thread::sleep_for(100ms);
            for (const auto& game : lister.transport->games()) {
                if (match(game)) {
                    found = game;
                    return true;
                }
            }
            return false;
        },
        timeout);
}

bool killProcess(unsigned long pid) {
#ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!process)
        return false;
    const bool ok = TerminateProcess(process, 1) != 0;
    CloseHandle(process);
    return ok;
#else
    return ::kill(static_cast<pid_t>(pid), SIGKILL) == 0;
#endif
}

void pair(Peer& a, Peer& b, const std::string& tag) {
    // Both directions at once: one side is the offerer and the other has to
    // ask, whichever way the ids happen to fall.
    const std::string ab = tag + " " + a.label + "->" + b.label;
    const std::string ba = tag + " " + b.label + "->" + a.label;
    a.send(b, snps::Channel::Unreliable, ab);
    b.send(a, snps::Channel::Unreliable, ba);

    // Storm retransmits; keep sending the way it would until something lands.
    bool gotAb = false;
    bool gotBa = false;
    const auto until = Clock::now() + 20s;
    while (Clock::now() < until && !(gotAb && gotBa)) {
        gotAb = gotAb || b.received(a, ab, 250ms);
        gotBa = gotBa || a.received(b, ba, 250ms);
        if (!gotAb)
            a.send(b, snps::Channel::Unreliable, ab);
        if (!gotBa)
            b.send(a, snps::Channel::Unreliable, ba);
    }
    check(gotAb && gotBa, a.label + " and " + b.label +
                              " reach each other in both directions at once");
}

// The way DevilutionX meets: no directory and no rooms. Every peer is open, a
// game-list query goes out by multicast and its answer comes back through the
// server, and then players connect to each other directly.
void openPeers(const std::string& server) {
    Peer asker("asker", server, true);
    Peer master("master", server, true);
    Peer closed("closed", server, false, true);  // same product, but not open
    check(online({&asker, &master, &closed}), "three transports come online, two of them open");
    if (asker.transport->status() != snps::Status::Online ||
        master.transport->status() != snps::Status::Online)
        return;

    // Two peers that never answer, checked at the end. The highest id there
    // is, which this side offers to; and the lowest, which it has to ask.
    snps::PeerId offeredTo{};
    offeredTo.fill(0xFF);
    snps::PeerId askedOf{};
    askedOf.back() = 1;
    const auto anyone = bytes("anyone there?");
    asker.transport->send(offeredTo, snps::Channel::Reliable, anyone.data(), anyone.size());
    asker.transport->send(askedOf, snps::Channel::Reliable, anyone.data(), anyone.size());
    const auto ghostsSent = Clock::now();

    // --- finding a game ---------------------------------------------------
    const auto query = bytes("who is hosting?");
    check(asker.transport->multicast(query.data(), query.size()), "multicast() is accepted");
    check(master.received(asker, "who is hosting?", 5s, &kServer),
          "an open peer hears the multicast, as Channel::Server");
    std::this_thread::sleep_for(300ms);
    check(!closed.has(asker, "who is hosting?"), "a peer that is not open does not");
    check(!asker.has(asker, "who is hosting?"), "nor does the sender");

    const auto answer = bytes("I am");
    check(master.transport->sendViaServer(asker.id(), answer.data(), answer.size()),
          "sendViaServer() is accepted");
    check(asker.received(master, "I am", 5s, &kServer),
          "the answer comes back through the server");
    check(!master.transport->connected(asker.id()) && master.transport->linkCount() == 0,
          "neither opened a peer connection for it");

    const auto unasked = bytes("unasked");
    closed.transport->sendViaServer(asker.id(), unasked.data(), unasked.size());
    master.transport->sendViaServer(closed.id(), unasked.data(), unasked.size());
    std::this_thread::sleep_for(300ms);
    check(asker.has(closed, "unasked") && !closed.has(master, "unasked"),
          "an open peer takes datagrams from anyone in scope; a closed one does not");

    std::vector<std::uint8_t> huge(snps::kMaxServerDatagram + 1, 'x');
    check(!master.transport->sendViaServer(asker.id(), huge.data(), huge.size()) &&
              !master.transport->multicast(huge.data(), huge.size()),
          "nothing over kMaxServerDatagram goes through the server");

    // --- joining: straight to a connection, no room between them -----------
    asker.send(master, snps::Channel::Reliable, "let me in");
    master.send(asker, snps::Channel::Reliable, "come in");
    check(master.received(asker, "let me in") && asker.received(master, "come in"),
          "open peers connect with no room, whichever of them offers");

    // --- leaving -----------------------------------------------------------
    asker.transport->disconnect(master.id());
    check(!asker.transport->connected(master.id()), "disconnect() closes the link at once");
    snps::PeerId who{};
    check(waitFor([&] { return master.transport->lost(who); }, 10s) && who == asker.id(),
          "the other side reports the link lost");

    asker.send(master, snps::Channel::Reliable, "back again");
    check(master.received(asker, "back again"), "the next send connects again");

    bool offeredLost = false;
    bool askedLost = false;
    bool masterLost = false;
    waitFor(
        [&] {
            snps::PeerId peer{};
            while (asker.transport->lost(peer)) {
                offeredLost = offeredLost || peer == offeredTo;
                askedLost = askedLost || peer == askedOf;
                masterLost = masterLost || peer == master.id();
            }
            return offeredLost && askedLost;
        },
        30s - std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - ghostsSent));
    check(offeredLost, "a peer that never answers our offer is given up on and reported lost");
    check(askedLost, "so is one we asked for an offer and never got one");
    check(!masterLost, "the link disconnect() closed is not");
}

// The signalling server dies mid-game. The links already open carry the game
// on, and nothing new can start until a server is back. With run.mjs's
// --respawn one is, two seconds later, on the same port and with the same
// secret. The transports reconnect by themselves, as the same peers, and put
// back the game and the membership the new server never heard of. Last, since
// it costs the server everything it knew.
void serverLoss(const std::string& server) {
    const char* pid = std::getenv("SNPS_SERVER_PID");
    if (!pid) {
        std::printf("  skip  losing the server: run.mjs did not say which process it is\n");
        return;
    }
    const bool respawns = std::getenv("SNPS_SERVER_RESPAWN") != nullptr;

    Peer host("host", server);
    Peer joiner("joiner", server);
    check(online({&host, &joiner}), "a host and a joiner come online");
    snps::Advert advert;
    advert.name = "survivor";
    advert.maxPlayers = 4;
    host.transport->advertise(advert);
    snps::Game game;
    check(findGame(joiner, [&](const snps::Game& g) { return g.name == advert.name; }, game, 5s),
          "the joiner finds the game");
    // The joiner speaks first: its join and its first signal share one
    // socket, so the server has made it a member before the host answers.
    joiner.transport->join(game.id);
    joiner.send(host, snps::Channel::Unreliable, "before");
    const bool reached = host.received(joiner, "before");
    host.send(joiner, snps::Channel::Reliable, "before");
    check(reached && joiner.received(host, "before"), "and they connect");
    const snps::PeerId hostId = host.id();
    const snps::PeerId joinerId = joiner.id();

    check(killProcess(std::strtoul(pid, nullptr, 10)), "the signalling server is killed");
    check(waitFor(
              [&] {
                  return status(host, snps::Status::Connecting) &&
                         status(joiner, snps::Status::Connecting);
              },
              5s),
          "both transports notice, and are Connecting again rather than Offline");

    host.send(joiner, snps::Channel::Reliable, "during");
    joiner.send(host, snps::Channel::Unreliable, "during");
    check(joiner.received(host, "during") && host.received(joiner, "during"),
          "open channels carry on without the server, both of them");

    snps::PeerId stranger{};
    stranger.fill(0xEE);
    const auto hello = bytes("hello");
    check(!host.transport->send(stranger, snps::Channel::Reliable, hello.data(), hello.size()),
          "but a new connection cannot start");
    check(!host.transport->sendViaServer(joinerId, hello.data(), hello.size()) &&
              !host.transport->multicast(hello.data(), hello.size()),
          "and nothing goes through the server");

    if (!respawns) {
        std::printf("  skip  coming back: run.mjs was not asked to --respawn the server\n");
        return;
    }

    check(waitFor(
              [&] {
                  return status(host, snps::Status::Online) &&
                         status(joiner, snps::Status::Online);
              },
              20s),
          "both reconnect by themselves once a new server is up");
    check(host.id() == hostId && joiner.id() == joinerId,
          "as the same peers: the new server honours the old one's resume tokens");

    snps::Game again;
    check(findGame(
              joiner, [&](const snps::Game& g) { return g.id == game.id && g.players == 2; },
              again, 15s),
          "the game is listed again under the same id, with the joiner back in it");

    // The host may signal the joiner only as a member of the same game, and
    // every connection needs it to, whichever of them offers. So this one
    // needs the joiner to have joined the new server's copy of the game.
    joiner.transport->disconnect(hostId);
    snps::PeerId who{};
    waitFor([&] { return host.transport->lost(who) && who == joinerId; }, 10s);
    joiner.send(host, snps::Channel::Reliable, "after");
    const bool there = host.received(joiner, "after");
    host.send(joiner, snps::Channel::Reliable, "after");
    check(there && joiner.received(host, "after"),
          "and a new connection between them goes through the new server");
}

void run(const std::string& server) {
    std::printf("snps harness, core %s, against %s\n\n", snps::CoreVersion(),
                server.c_str());

    snps::initialize();

    {
        Peer host("host", server);
        Peer joiner("joiner", server);
        host.transport->start();
        joiner.transport->start();

        check(host.transport->waitOnline(5s) && joiner.transport->waitOnline(5s),
              "both transports come online");
        if (host.transport->status() != snps::Status::Online ||
            joiner.transport->status() != snps::Status::Online) {
            std::printf("  (%s / %s)\n", host.transport->error().c_str(),
                        joiner.transport->error().c_str());
            return;
        }
        check(host.id() != joiner.id(), "they get distinct peer ids");

        // --- the directory ---------------------------------------------------
        // StarCraft 1.16.1's own stat string, CRs and all, and a name with a
        // byte above 0x7F as a Windows code page would produce. Both have to
        // come back byte for byte: a stripped CR made the game hide the listing.
        snps::Advert advert;
        advert.name = "harness game \xE9\xFF";
        advert.stat = ",44,,6,,f,,4,5ab5438b,10,,Chaos\rUntitled Scenario\r";
        advert.state = 7;
        advert.maxPlayers = 4;
        advert.extra = {1, 2, 3, 4, 5};
        host.transport->advertise(advert);

        snps::Game game;
        const bool listed = waitFor(
            [&] {
                joiner.transport->refreshGames();
                std::this_thread::sleep_for(100ms);
                for (const auto& g : joiner.transport->games()) {
                    if (g.name == advert.name) {
                        game = g;
                        return true;
                    }
                }
                return false;
            },
            5s);
        check(listed, "the joiner finds the advertised game");
        check(game.host == host.id(), "the listing names the host's peer id");
        check(game.stat == advert.stat, "StarCraft's stat string survives byte for byte, CRs included");
        check(game.name == advert.name, "so do name bytes above 0x7F");
        check(game.state == advert.state && game.maxPlayers == advert.maxPlayers,
              "state and player limit survive the round trip");
        check(game.extra == advert.extra, "the extra bytes survive base64 both ways");

        joiner.transport->join(game.id);

        // --- first contact, the way storm makes it -----------------------------
        // Several datagrams before any connection exists: they have to be held
        // and delivered once the channel opens, not dropped.
        for (int i = 0; i < 5; ++i)
            joiner.send(host, snps::Channel::Unreliable, "early " + std::to_string(i));
        bool allEarly = true;
        for (int i = 0; i < 5; ++i)
            allEarly = host.received(joiner, "early " + std::to_string(i)) && allEarly;
        check(allEarly, "datagrams sent before the connection exists arrive once it opens");
        check(host.inbox.front().from == joiner.id(),
              "the sender is identified by its peer id");
        check(host.wakeups > 0, "onReceivable fires when datagrams are queued");

        host.send(joiner, snps::Channel::Unreliable, "welcome aboard");
        check(joiner.received(host, "welcome aboard"), "the host replies without a new handshake");
        check(host.transport->connected(joiner.id()) &&
                  joiner.transport->connected(host.id()),
              "both sides report the link open");

        // --- the reliable channel ---------------------------------------------
        const int count = 200;
        for (int i = 0; i < count; ++i)
            joiner.send(host, snps::Channel::Reliable, "r" + std::to_string(i));
        const bool allReliable = waitFor(
            [&] {
                host.collect();
                int seen = 0;
                for (const auto& d : host.inbox)
                    if (d.channel == snps::Channel::Reliable)
                        ++seen;
                return seen >= count;
            },
            15s);
        std::vector<std::string> order;
        for (const auto& d : host.inbox)
            if (d.channel == snps::Channel::Reliable)
                order.push_back(text(d.data));
        bool inOrder = order.size() == static_cast<std::size_t>(count);
        for (int i = 0; inOrder && i < count; ++i)
            inOrder = order[i] == "r" + std::to_string(i);
        check(allReliable && inOrder,
              std::to_string(count) + " reliable messages arrive complete and in order");

        std::string big(500, 'x');
        joiner.send(host, snps::Channel::Unreliable, big);
        check(host.received(joiner, big), "a 500-byte datagram, storm's largest, arrives intact");

        // --- loopback -----------------------------------------------------------
        host.send(host, snps::Channel::Unreliable, "to myself");
        check(host.received(host, "to myself"), "a datagram to our own id comes back");

        // --- a third player, met through gossip ---------------------------------
        // Storm tells everyone about a new player by passing its address around,
        // then each of them sends to it with no introduction.
        {
            Peer third("third", server);
            third.transport->start();
            check(third.transport->waitOnline(5s), "a third transport comes online");
            third.transport->join(game.id);
            std::this_thread::sleep_for(200ms);

            pair(third, joiner, "gossip");
            pair(third, host, "gossip");
            check(third.transport->linkCount() == 2, "the third player holds two links");
        }
        // The third player is gone; the others must carry on regardless.
        joiner.send(host, snps::Channel::Unreliable, "still here");
        check(host.received(joiner, "still here"), "losing a player does not disturb the rest");

        // --- the host leaves ------------------------------------------------------
        host.transport->withdraw();
        const bool gone = waitFor(
            [&] {
                joiner.transport->refreshGames();
                std::this_thread::sleep_for(100ms);
                for (const auto& g : joiner.transport->games())
                    if (g.id == game.id)
                        return false;
                return true;
            },
            5s);
        check(gone, "a withdrawn game leaves the listing");
        check(host.transport->dropped() == 0 && joiner.transport->dropped() == 0,
              "nothing was dropped along the way");
    }

    snps::shutdown();
    check(true, "shutdown() returns with every transport destroyed");

    // Storm unloads and reloads the module on every visit to the network menu.
    snps::initialize();
    {
        Peer again("again", server);
        again.transport->start();
        check(again.transport->waitOnline(5s), "a second initialize() works as well as the first");
    }

    // Only a transport that has been welcomed reconnects. Before that a
    // failure is final, so storm's Initialize hears at once that the network
    // is unavailable rather than waiting out its timeout.
    {
        snps::Options options;
        options.server = "ws://127.0.0.1:1";  // nothing listens there
        options.product = "SEXP";
        snps::Transport nowhere(options);
        const auto began = Clock::now();
        nowhere.start();
        const bool up = nowhere.waitOnline(10s);
        check(!up && nowhere.status() == snps::Status::Offline && Clock::now() - began < 8s,
              "a server that cannot be reached is Offline straight away, and not retried");

        options.server = server;
        options.product = "S!";  // not a product code: the hello is refused
        snps::Transport refused(options);
        refused.start();
        check(!refused.waitOnline(5s) && refused.status() == snps::Status::Offline &&
                  refused.error().find("refused") != std::string::npos,
              "so is a transport whose hello the server refuses");
    }
    snps::shutdown();

    std::printf("\nopen peers, the way DevilutionX meets\n");
    snps::initialize();
    openPeers(server);
    serverLoss(server);
    snps::shutdown();
}

} // namespace

int main(int argc, char** argv) {
    std::string server = "ws://127.0.0.1:8765";
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--server") == 0 && i + 1 < argc)
            server = argv[++i];
        else if (std::strcmp(argv[i], "--verbose") == 0)
            verbose = true;
    }

    const auto started = Clock::now();
    std::mutex logMutex;
    snps::setLogSink([&](snps::LogLevel level, const std::string& line) {
        if (!verbose && level < snps::LogLevel::Warning)
            return;
        std::lock_guard<std::mutex> lock(logMutex);
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
        std::fprintf(stderr, "    [%6lld] %s\n", static_cast<long long>(ms.count()),
                     line.c_str());
    });

    run(server);

    snps::setLogSink(nullptr);
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
