// The core in WebAssembly, used the way DevilutionX's web build uses it: this
// file is compiled with -fno-exceptions, the program is linked with -sASYNCIFY,
// and every wait is an emscripten_sleep, which is what SDL_Delay becomes there.
//
//   node snps-wasm-test.js --server ws://127.0.0.1:8765
//       Two open transports: a multicast, and an answer through the server.
//       Node has no RTCPeerConnection, so the one peer connection attempted
//       has to fail inside datachannel-wasm without taking the program down.
//       Under run.mjs --respawn, it then kills the server, and both have to
//       reconnect to the one that replaces it -- with no thread, only the
//       calls a game would make anyway.
//
//   test.html?server=ws://127.0.0.1:8765&echo=1&restart=1   (in a browser)
//       One transport, which finds snps-echo (tests/harness) by multicast and
//       then talks to it over both data channels: the browser's WebRTC
//       against libdatachannel. With restart, browser.mjs then replaces the
//       signalling server, and the browser has to reconnect, find the echo
//       peer reconnected too, and connect to it again. browser.mjs sets all
//       of that up.

#include "snps/transport.hpp"

#include <emscripten.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;
int g_unconnectable = 0;  // "cannot connect" / "cannot answer" in the log

void check(bool ok, const std::string& what) {
    ++g_checks;
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok)
        ++g_failures;
}

std::vector<std::uint8_t> bytes(const std::string& text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::string text(const std::vector<std::uint8_t>& data) {
    return std::string(data.begin(), data.end());
}

snps::Options options(const std::string& server, const char* name) {
    snps::Options o;
    o.server = server;
    o.product = "DVLX";
    o.version = 1;
    o.name = name;
    o.open = true;
    return o;
}

// A transport and everything it has received, waited on the way the game waits.
struct Peer {
    snps::Transport transport;
    std::vector<snps::Datagram> inbox;

    explicit Peer(snps::Options o) : transport(std::move(o)) {}

    void collect() {
        snps::Datagram d;
        while (transport.receive(d))
            inbox.push_back(std::move(d));
    }

    template <class Done>
    bool await(Done done, int ms) {
        for (int waited = 0;; waited += 10) {
            collect();
            if (done())
                return true;
            if (waited >= ms)
                return false;
            emscripten_sleep(10);
        }
    }

    bool has(const snps::PeerId& from, snps::Channel channel, const std::string& payload) {
        for (const auto& d : inbox)
            if (d.from == from && d.channel == channel && text(d.data) == payload)
                return true;
        return false;
    }

    bool online() {
        await([&] { return transport.status() != snps::Status::Idle &&
                           transport.status() != snps::Status::Connecting; },
              5000);
        return transport.status() == snps::Status::Online;
    }
};

// DevilutionX makes a fresh transport for each game, and destroys the lobby's
// while its WebSocket may still be connecting. Nothing may throw, and in a
// browser nothing should reach the console either (README.md).
void destroyedWhileConnecting(const std::string& server) {
    {
        snps::Transport brief(options(server, "brief"));
        brief.start();
    }
    emscripten_sleep(200);  // for whatever the socket does next; an exception fails the page
    check(true, "a transport destroyed while still connecting throws nothing");
}

void serverPath(const std::string& server) {
    Peer asker(options(server, "asker"));
    Peer master(options(server, "master"));
    asker.transport.start();
    master.transport.start();
    check(asker.online() && master.online(), "two open transports come online");
    if (g_failures)
        return;
    const snps::PeerId askerId = *asker.transport.self();
    const snps::PeerId masterId = *master.transport.self();

    const auto query = bytes("who is hosting?");
    check(asker.transport.multicast(query.data(), query.size()), "multicast() is accepted");
    check(master.await([&] { return master.has(askerId, snps::Channel::Server, "who is hosting?"); },
                       5000),
          "the multicast arrives, as Channel::Server");

    const auto answer = bytes("I am");
    check(master.transport.sendViaServer(askerId, answer.data(), answer.size()),
          "sendViaServer() is accepted");
    check(asker.await([&] { return asker.has(masterId, snps::Channel::Server, "I am"); }, 5000),
          "the answer comes back through the server");

    // Node has no RTCPeerConnection: datachannel-wasm throws when asked for
    // one, and the core has to catch that with -fexceptions while this file
    // has none. Both send, so whichever has the lower id offers, and throws.
    const auto hello = bytes("hello");
    master.transport.send(askerId, snps::Channel::Reliable, hello.data(), hello.size());
    asker.transport.send(masterId, snps::Channel::Reliable, hello.data(), hello.size());
    emscripten_sleep(100);
    check(g_unconnectable > 0 && asker.transport.status() == snps::Status::Online &&
              master.transport.status() == snps::Status::Online,
          "an exception from datachannel-wasm is caught, and both carry on");

    // The server dies, and run.mjs --respawn starts another on the same port,
    // with the same secret, two seconds later. Only Node can kill it.
    const int pid = EM_ASM_INT({
        return typeof process === 'object' && process.env.SNPS_SERVER_RESPAWN
                   ? Number(process.env.SNPS_SERVER_PID) : 0;
    });
    if (!pid) {
        std::printf("  skip  losing the server: run.mjs was not asked to --respawn it\n");
        return;
    }
    EM_ASM({ process.kill($0); }, pid);
    const auto both = [&](snps::Status status) {
        return asker.transport.status() == status && master.transport.status() == status;
    };
    check(asker.await([&] { master.collect(); return both(snps::Status::Connecting); }, 5000),
          "both notice the server is gone, and are Connecting again");
    check(asker.await([&] { master.collect(); return both(snps::Status::Online); }, 20000),
          "both reconnect by themselves once another server is up");
    check(*asker.transport.self() == askerId && *master.transport.self() == masterId,
          "as the same peers");
    const auto later = bytes("still hosting?");
    asker.transport.multicast(later.data(), later.size());
    check(master.await([&] { return master.has(askerId, snps::Channel::Server, "still hosting?"); },
                       5000),
          "and the server path works again");
}

void dataChannels(const std::string& server, bool restart) {
    Peer browser(options(server, "browser"));
    browser.transport.start();
    check(browser.online(), "the browser's transport comes online");
    if (g_failures)
        return;

    // Finding the echo peer the way DevilutionX finds a game: ask everyone.
    snps::PeerId echo{};
    bool found = false;
    const auto query = bytes("anyone there?");
    for (int tries = 0; tries < 20 && !found; ++tries) {
        browser.transport.multicast(query.data(), query.size());
        found = browser.await(
            [&] {
                for (const auto& d : browser.inbox)
                    if (d.channel == snps::Channel::Server &&
                        text(d.data) == "echo: anyone there?") {
                        echo = d.from;
                        return true;
                    }
                return false;
            },
            500);
    }
    check(found, "the native echo peer answers the multicast through the server");
    if (!found)
        return;

    // Held while the connection is made, then sent in order: the browser's
    // SCTP to libdatachannel's, and back.
    const int count = 50;
    for (int i = 0; i < count; ++i) {
        const auto message = bytes("reliable " + std::to_string(i));
        browser.transport.send(echo, snps::Channel::Reliable, message.data(), message.size());
    }
    std::vector<std::string> order;
    const bool all = browser.await(
        [&] {
            order.clear();
            for (const auto& d : browser.inbox)
                if (d.from == echo && d.channel == snps::Channel::Reliable)
                    order.push_back(text(d.data));
            return order.size() >= static_cast<std::size_t>(count);
        },
        20000);
    bool inOrder = all && order.size() == static_cast<std::size_t>(count);
    for (int i = 0; inOrder && i < count; ++i)
        inOrder = order[i] == "reliable " + std::to_string(i);
    check(inOrder, "50 reliable messages reach the native peer and come back, in order");
    check(browser.transport.connected(echo), "connected() reports the link open");

    // Unreliable really is: keep sending, as storm would, until one returns.
    bool unreliable = false;
    const auto ping = bytes("unreliable");
    for (int i = 0; i < 20 && !unreliable; ++i) {
        browser.transport.send(echo, snps::Channel::Unreliable, ping.data(), ping.size());
        unreliable = browser.await(
            [&] { return browser.has(echo, snps::Channel::Unreliable, "unreliable"); }, 250);
    }
    check(unreliable, "the unreliable channel works both ways too");

    // Leaving and coming back, as a DevilutionX player does between games.
    browser.transport.disconnect(echo);
    check(!browser.transport.connected(echo), "disconnect() closes the link");
    const auto again = bytes("back again");
    browser.transport.send(echo, snps::Channel::Reliable, again.data(), again.size());
    check(browser.await([&] { return browser.has(echo, snps::Channel::Reliable, "back again"); },
                        20000),
          "the next send connects again");

    if (!restart)
        return;
    // browser.mjs kills the signalling server when asked, and starts another
    // on the same port, with the same secret, two seconds later.
    const snps::PeerId self = *browser.transport.self();
    EM_ASM({ fetch('/restart-signalling'); });
    const auto is = [&](snps::Status status) { return browser.transport.status() == status; };
    check(browser.await([&] { return is(snps::Status::Connecting); }, 5000),
          "the signalling server dies, and the browser is Connecting again");
    check(browser.await([&] { return is(snps::Status::Online); }, 20000),
          "it reconnects by itself once another server is up");
    check(*browser.transport.self() == self, "as the same peer");

    // The echo peer reconnects on a schedule of its own; it is back when it
    // answers a multicast again, from the same id.
    bool back = false;
    const auto still = bytes("still there?");
    for (int tries = 0; tries < 40 && !back; ++tries) {
        browser.transport.multicast(still.data(), still.size());
        back = browser.await(
            [&] { return browser.has(echo, snps::Channel::Server, "echo: still there?"); }, 500);
    }
    check(back, "so does the native peer, as the same peer");

    browser.transport.disconnect(echo);
    const auto after = bytes("after the restart");
    browser.transport.send(echo, snps::Channel::Reliable, after.data(), after.size());
    check(browser.await(
              [&] { return browser.has(echo, snps::Channel::Reliable, "after the restart"); }, 20000),
          "and they connect again, through the new server");
}

} // namespace

int main(int argc, char** argv) {
    std::string server = "ws://127.0.0.1:8765";
    bool echo = false;
    bool restart = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--server") == 0 && i + 1 < argc)
            server = argv[++i];
        else if (std::strcmp(argv[i], "--echo") == 0 && i + 1 < argc)
            echo = std::strcmp(argv[++i], "0") != 0;
        else if (std::strcmp(argv[i], "--restart") == 0 && i + 1 < argc)
            restart = std::strcmp(argv[++i], "0") != 0;
    }

    snps::setLogSink([](snps::LogLevel level, const std::string& line) {
        if (line.rfind("cannot connect", 0) == 0 || line.rfind("cannot answer", 0) == 0)
            ++g_unconnectable;
        if (level >= snps::LogLevel::Warning)
            std::printf("    [%s] %s\n", level == snps::LogLevel::Error ? "error" : "warning",
                        line.c_str());
    });
    std::printf("snps wasm test against %s, %s\n\n", server.c_str(),
                echo ? "data channels to snps-echo" : "the server path");

    snps::initialize();
    destroyedWhileConnecting(server);
    if (echo)
        dataChannels(server, restart);
    else
        serverPath(server);
    snps::shutdown();
    snps::setLogSink(nullptr);

    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
