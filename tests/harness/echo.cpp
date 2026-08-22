// A peer that answers everything, for testing another build of the core
// against this one: tests/wasm runs the browser's against it.
//
//     snps-echo --server ws://127.0.0.1:8765 [--seconds 600] [--verbose]
//
// Open, with DevilutionX's product and version. A datagram through the server
// -- a multicast, or one sent to it -- gets an answer through the server:
// "echo: " and the text. A datagram on a peer connection comes straight back,
// on the channel it came on.

#include "snps/transport.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

using namespace std::chrono_literals;

int main(int argc, char** argv) {
    std::string server = "ws://127.0.0.1:8765";
    long seconds = 600;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--server") == 0 && i + 1 < argc)
            server = argv[++i];
        else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            seconds = std::strtol(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--verbose") == 0)
            verbose = true;
    }

    std::mutex logMutex;
    snps::setLogSink([&](snps::LogLevel level, const std::string& line) {
        if (!verbose && level < snps::LogLevel::Info)
            return;
        std::lock_guard<std::mutex> lock(logMutex);
        std::printf("  [log] %s\n", line.c_str());
        std::fflush(stdout);
    });

    int status = 0;
    snps::initialize();
    {
        snps::Options options;
        options.server = server;
        options.product = "DVLX";
        options.version = 1;
        options.name = "echo";
        options.open = true;
        snps::Transport transport(options);
        transport.start();
        if (!transport.waitOnline(5s)) {
            std::printf("echo: not online: %s\n", transport.error().c_str());
            status = 1;
        } else {
            std::printf("echo: %s online at %s\n", snps::toHex(*transport.self()).c_str(),
                        server.c_str());
            std::fflush(stdout);

            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
            unsigned long echoed = 0;
            while (std::chrono::steady_clock::now() < until) {
                snps::Datagram d;
                while (transport.receive(d)) {
                    if (d.channel == snps::Channel::Server) {
                        std::string reply = "echo: ";
                        reply.append(d.data.begin(), d.data.end());
                        transport.sendViaServer(d.from,
                                                reinterpret_cast<const std::uint8_t*>(reply.data()),
                                                reply.size());
                    } else {
                        transport.send(d.from, d.channel, d.data.data(), d.data.size());
                    }
                    if (++echoed % 100 == 1 || verbose) {
                        std::printf("echo: %lu datagram(s) answered, the last from %s\n", echoed,
                                    snps::toHex(d.from).substr(0, 8).c_str());
                        std::fflush(stdout);
                    }
                }
                snps::PeerId lost{};
                while (transport.lost(lost)) {
                    std::printf("echo: lost the link to %s\n",
                                snps::toHex(lost).substr(0, 8).c_str());
                    std::fflush(stdout);
                }
                std::this_thread::sleep_for(5ms);
            }
        }
    }
    snps::shutdown();
    return status;
}
