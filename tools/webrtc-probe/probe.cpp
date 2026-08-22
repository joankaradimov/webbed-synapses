// Does the WebRTC stack actually work as 32-bit x86?
//
// Building is only half the question. This opens a real peer connection
// between two in-process peers -- ICE, DTLS, SCTP, the lot -- over the two
// data channels the transport core needs (AGENT.md section 4):
//
//   "storm"  unreliable, unordered   -- storm sequences and retransmits itself
//   "dvl"    reliable, ordered       -- what base_protocol::send assumes
//
// Signalling is done by handing the descriptions and candidates straight to
// the other object, so this needs no server and no network beyond loopback.
//
// It also exercises Preload()/Cleanup() twice, because storm LoadLibrary's and
// FreeLibrary's every *.snp in the game directory on each trip to the network
// menu (docs/webrtc-stack.md) -- a stack that only initialises once is a stack
// that hangs the second time the player opens that menu.

#include <rtc/rtc.hpp>

#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string &what) {
    std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
    if (!ok)
        ++failures;
}

// A promise fulfilled from a libdatachannel callback thread.
struct Signal {
    std::promise<std::string> promise;
    std::future<std::string> future = promise.get_future();
    bool done = false;

    void set(std::string value) {
        if (!done) {
            done = true;
            promise.set_value(std::move(value));
        }
    }

    bool wait(int seconds, std::string *out = nullptr) {
        if (future.wait_for(std::chrono::seconds(seconds)) != std::future_status::ready)
            return false;
        std::string value = future.get();
        if (out)
            *out = value;
        return true;
    }
};

std::string text_of(const rtc::message_variant &message) {
    if (std::holds_alternative<std::string>(message))
        return std::get<std::string>(message);
    const auto &binary = std::get<rtc::binary>(message);
    return "<" + std::to_string(binary.size()) + " binary bytes>";
}

} // namespace

int main() {
    std::cout << "libdatachannel probe, pointer size " << sizeof(void *)
              << " bytes\n" << std::endl;

    rtc::InitLogger(rtc::LogLevel::Error);
    rtc::Preload();

    // No ICE servers: both peers are in this process, so host candidates on
    // loopback are all that is needed and nothing leaves the machine.
    rtc::Configuration config;

    auto offerer = std::make_shared<rtc::PeerConnection>(config);
    auto answerer = std::make_shared<rtc::PeerConnection>(config);

    Signal offerer_connected, answerer_connected;
    Signal storm_received, dvl_received, reply_received;

    // -- signalling, such as it is -------------------------------------
    // Raw pointers on purpose: both connections outlive every callback, and
    // capturing the shared_ptrs would make the two hold each other forever.
    auto *peer_a = offerer.get();
    auto *peer_b = answerer.get();

    offerer->onLocalDescription([peer_b](rtc::Description description) {
        peer_b->setRemoteDescription(
            rtc::Description(std::string(description), description.typeString()));
    });
    answerer->onLocalDescription([peer_a](rtc::Description description) {
        peer_a->setRemoteDescription(
            rtc::Description(std::string(description), description.typeString()));
    });
    offerer->onLocalCandidate([peer_b](rtc::Candidate candidate) {
        peer_b->addRemoteCandidate(rtc::Candidate(std::string(candidate),
                                                  candidate.mid()));
    });
    answerer->onLocalCandidate([peer_a](rtc::Candidate candidate) {
        peer_a->addRemoteCandidate(rtc::Candidate(std::string(candidate),
                                                  candidate.mid()));
    });

    offerer->onStateChange([&](rtc::PeerConnection::State state) {
        if (state == rtc::PeerConnection::State::Connected)
            offerer_connected.set("connected");
        else if (state == rtc::PeerConnection::State::Failed)
            offerer_connected.set("failed");
    });
    answerer->onStateChange([&](rtc::PeerConnection::State state) {
        if (state == rtc::PeerConnection::State::Connected)
            answerer_connected.set("connected");
        else if (state == rtc::PeerConnection::State::Failed)
            answerer_connected.set("failed");
    });

    // -- the answerer's side of the channels ---------------------------
    std::vector<std::shared_ptr<rtc::DataChannel>> accepted;
    answerer->onDataChannel([&](std::shared_ptr<rtc::DataChannel> channel) {
        accepted.push_back(channel);
        const std::string label = channel->label();
        channel->onMessage([&, channel, label](rtc::message_variant message) {
            if (label == "storm")
                storm_received.set(text_of(message));
            else
                dvl_received.set(text_of(message));
            channel->send("ack:" + label);
        });
    });

    // -- the offerer's channels ----------------------------------------
    // Creating a data channel is what makes libdatachannel produce an offer,
    // so the two descriptions above start flowing here.
    rtc::DataChannelInit unreliable;
    unreliable.reliability.unordered = true;
    unreliable.reliability.maxRetransmits = 0;
    auto storm = offerer->createDataChannel("storm", unreliable);

    auto dvl = offerer->createDataChannel("dvl");   // reliable and ordered

    Signal storm_open, dvl_open;
    storm->onOpen([&]() { storm_open.set("open"); });
    dvl->onOpen([&]() { dvl_open.set("open"); });
    storm->onMessage([&](rtc::message_variant message) {
        reply_received.set(text_of(message));
    });

    // -- and see whether any of it works -------------------------------
    std::string state;
    check(offerer_connected.wait(20, &state) && state == "connected",
          "the offerer reaches Connected");
    check(answerer_connected.wait(20, &state) && state == "connected",
          "the answerer reaches Connected");
    check(storm_open.wait(10), "the unreliable channel opens");
    check(dvl_open.wait(10), "the reliable channel opens");

    check(storm->reliability().unordered && storm->reliability().maxRetransmits
              && *storm->reliability().maxRetransmits == 0,
          "the unreliable channel negotiated unordered, zero retransmits");
    check(!dvl->reliability().unordered && !dvl->reliability().maxRetransmits,
          "the reliable channel stayed ordered and reliable");

    const std::string storm_payload(200, 'S');   // a storm-sized datagram
    const std::string dvl_payload(4000, 'D');    // a DevilutionX-sized message
    storm->send(storm_payload);
    dvl->send(dvl_payload);

    std::string got;
    check(storm_received.wait(10, &got) && got == storm_payload,
          "200 bytes arrive over the unreliable channel");
    check(dvl_received.wait(10, &got) && got == dvl_payload,
          "4000 bytes arrive over the reliable channel");
    check(reply_received.wait(10, &got) && got == "ack:storm",
          "a reply comes back the other way");

    if (auto local = offerer->localAddress(), remote = offerer->remoteAddress();
        local && remote) {
        std::cout << "\n  selected pair: " << *local << " -> " << *remote
                  << std::endl;
    }
    std::cout << "  max message size: " << storm->maxMessageSize() << " bytes"
              << std::endl;

    accepted.clear();
    storm.reset();
    dvl.reset();
    offerer.reset();
    answerer.reset();
    rtc::Cleanup();
    check(true, "Cleanup() returns without hanging");

    // Storm unloads and reloads every module each time the player opens the
    // network menu, so the second initialisation matters as much as the first.
    rtc::Preload();
    { rtc::PeerConnection again{config}; }
    rtc::Cleanup();
    check(true, "a second Preload()/Cleanup() cycle survives");

    std::cout << "\n" << (failures ? "FAILED" : "all checks passed") << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
