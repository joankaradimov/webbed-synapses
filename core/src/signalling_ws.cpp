// The signalling connection as a WebSocket: libdatachannel's natively,
// datachannel-wasm's -- the browser's own -- in WebAssembly.

#include "signalling.hpp"

#include <rtc/rtc.hpp>

#include <atomic>
#include <chrono>
#include <mutex>

namespace snps::detail {

namespace {

using namespace std::chrono_literals;

#ifndef __EMSCRIPTEN__
// Natively, the WebSocket pings after this much silence from the server, and
// gives up after this many pings go unanswered. So a server that vanished
// without closing anything -- a machine that lost power, a NAT that forgot the
// connection -- is noticed within half a minute. A browser's WebSocket cannot
// ping, and notices only when TCP does.
constexpr auto kServerPingInterval = 10s;
constexpr int kServerPingsUnanswered = 2;
#endif

class WebSocketConnection final : public SignallingConnection,
                                  public std::enable_shared_from_this<WebSocketConnection> {
public:
    WebSocketConnection() {
#ifndef __EMSCRIPTEN__
        rtc::WebSocketConfiguration config;
        config.pingInterval = kServerPingInterval;
        config.maxOutstandingPings = kServerPingsUnanswered;
        ws_ = std::make_shared<rtc::WebSocket>(config);
#else
        ws_ = std::make_shared<rtc::WebSocket>();
#endif
    }

    void open(const std::string& url, Callbacks callbacks) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callbacks_ = std::move(callbacks);
        }
        std::weak_ptr<WebSocketConnection> weak = shared_from_this();
        ws_->onOpen([weak] {
            if (auto self = weak.lock())
                if (auto opened = self->callback(&Callbacks::opened))
                    opened();
        });
        ws_->onMessage([](rtc::binary) {},
                       [weak](std::string text) {
                           if (auto self = weak.lock())
                               if (auto message = self->callback(&Callbacks::message))
                                   message(text);
                       });
        ws_->onError([weak](std::string what) {
            if (auto self = weak.lock())
                self->finish("signalling error: " + what);
        });
        ws_->onClosed([weak] {
            if (auto self = weak.lock())
                self->finish("signalling connection closed");
        });
        try {
            ws_->open(url);
        } catch (const std::exception& e) {
            finish(std::string("cannot open signalling connection: ") + e.what());
        }
    }

    bool isOpen() const override { return ws_->isOpen(); }

    void send(const std::string& text) override {
        if (ws_->isOpen())
            ws_->send(text);
    }

    void close() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callbacks_ = {};
            done_ = true;
        }
        try {
            ws_->close();
        } catch (...) {
        }
    }

private:
    template <typename F>
    F callback(F Callbacks::*which) {
        std::lock_guard<std::mutex> lock(mutex_);
        return done_ ? F() : callbacks_.*which;
    }

    // An error is followed by a close, and either can come alone: report the
    // first, once.
    void finish(const std::string& why) {
        std::function<void(const std::string&)> closed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (done_)
                return;
            done_ = true;
            closed = std::move(callbacks_.closed);
            callbacks_ = {};
        }
        if (closed)
            closed(why);
    }

    std::shared_ptr<rtc::WebSocket> ws_;
    std::mutex mutex_;
    Callbacks callbacks_;
    bool done_ = false;
};

} // namespace

std::shared_ptr<SignallingConnection> makeWebSocketConnection() {
    return std::make_shared<WebSocketConnection>();
}

} // namespace snps::detail
