// The signalling connection over HTTP, for a server behind a host that will
// not upgrade a connection to a WebSocket (docs/signalling-protocol.md, "Over
// HTTP"; server/src/http.js is the other end).
//
//   POST {url}/session                     a session: the HTTP connection
//   GET  {url}/session/ID/events?after=N   the server's frames, as SSE
//   POST {url}/session/ID                  ours, one per line
//   POST {url}/session/ID/close            goodbye
//
// Streams end by design -- each after about 25 seconds, so that no proxy
// times them out -- and the next starts from the last event that arrived.
// The server keeps what was not acknowledged, so a stream cut short loses
// nothing. A stream that has not said `ready` within a few seconds is being
// held back by a proxy that buffers responses: from then on each request is
// a long poll, answered as soon as there is anything to say.
//
// Frames go up one POST at a time, each carrying whatever queued up behind
// the last, so they reach the server in the order they were sent.
//
// Over http_client.hpp, which is all a platform has to provide.

#include "signalling.hpp"

#include "http_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace snps::detail {

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// A stream says `ready` at once; one that has not by now is buffered.
constexpr auto kReadyPatience = 3s;

// Server-Sent Events, the parts of the format the server uses.
class EventParser {
public:
    struct Event {
        std::string type = "message";
        std::string data;
        std::string id;
        bool hasData = false;
    };

    template <typename F>
    void feed(const char* data, std::size_t size, F&& dispatch) {
        buffer_.append(data, size);
        std::size_t start = 0;
        for (;;) {
            const auto end = buffer_.find('\n', start);
            if (end == std::string::npos)
                break;
            std::string line = buffer_.substr(start, end - start);
            start = end + 1;
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty()) {
                if (event_.hasData || event_.type != "message")
                    dispatch(event_);
                event_ = {};
                continue;
            }
            if (line[0] == ':')
                continue;  // a comment: the keepalive
            const auto colon = line.find(':');
            const std::string field = line.substr(0, colon);
            std::string value = colon == std::string::npos ? "" : line.substr(colon + 1);
            if (!value.empty() && value[0] == ' ')
                value.erase(0, 1);
            if (field == "data") {
                if (event_.hasData)
                    event_.data += '\n';
                event_.data += value;
                event_.hasData = true;
            } else if (field == "event") {
                event_.type = value;
            } else if (field == "id") {
                event_.id = value;
            }
        }
        buffer_.erase(0, start);
    }

private:
    std::string buffer_;
    Event event_;
};

class HttpConnection final : public SignallingConnection,
                             public std::enable_shared_from_this<HttpConnection> {
public:
    explicit HttpConnection(bool poll) : poll_(poll) {}

    void open(const std::string& url, Callbacks callbacks) override {
        std::string base = url;
        while (!base.empty() && base.back() == '/')
            base.pop_back();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            base_ = base;
            callbacks_ = std::move(callbacks);
        }

        auto reply = std::make_shared<std::string>();
        auto status = std::make_shared<int>(0);
        std::weak_ptr<HttpConnection> weak = shared_from_this();
        http::Handlers handlers;
        handlers.response = [status](int code) { *status = code; };
        handlers.body = [reply](const char* data, std::size_t size) { reply->append(data, size); };
        handlers.done = [weak, reply, status](const std::string& error) {
            if (auto self = weak.lock())
                self->created(error, *status, *reply);
        };
        auto request = http::start("POST", base + "/session", "", std::move(handlers));
        if (!request) {
            finish("this build has no HTTP client");
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        requests_.push_back(request);
    }

    bool isOpen() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return open_ && !done_;
    }

    void send(const std::string& text) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!open_ || done_)
                return;
            outbox_.push_back(text);
        }
        pump();
    }

    void close() override {
        std::string goodbye;
        std::deque<std::shared_ptr<http::Request>> requests;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (done_)
                return;
            done_ = true;
            callbacks_ = {};
            if (open_)
                goodbye = base_ + "/session/" + session_ + "/close";
            requests.swap(requests_);
        }
        for (auto& request : requests)
            request->cancel();
        // So the server drops us at once, rather than after its grace period.
        // Nothing waits for the answer.
        if (!goodbye.empty())
            http::start("POST", goodbye, "", {});
    }

    void tick() override {
        bool restart = false;
        std::function<void(const std::string&)> note;
        std::function<void()> buffered;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!open_ || done_ || poll_ || ready_ || !streamStarted_)
                return;
            if (Clock::now() - *streamStarted_ < kReadyPatience)
                return;
            poll_ = true;  // and for the rest of this connection
            restart = true;
            note = callbacks_.note;
            buffered = callbacks_.buffered;
        }
        if (note)
            note("the event stream is being held back by something in between; long polling "
                 "instead");
        if (buffered)
            buffered();
        if (restart)
            stream();  // the buffered one is let go of
    }

private:
    // The session exists, or could not be made.
    void created(const std::string& error, int status, const std::string& reply) {
        std::string why;
        if (!error.empty()) {
            why = "cannot reach the signalling server over HTTP: " + error;
        } else if (status != 201) {
            why = "the signalling server answered " + std::to_string(status) +
                  " to POST /session: it may not speak HTTP, or the url may be wrong";
        } else {
            try {
                const auto session = nlohmann::json::parse(reply).value("session", "");
                if (session.empty())
                    why = "the signalling server sent no session";
                std::lock_guard<std::mutex> lock(mutex_);
                session_ = session;
            } catch (const std::exception& e) {
                why = std::string("unreadable answer to POST /session: ") + e.what();
            }
        }
        if (!why.empty()) {
            finish(why);
            return;
        }

        std::function<void()> opened;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (done_)
                return;
            open_ = true;
            opened = callbacks_.opened;
        }
        stream();
        if (opened)
            opened();
    }

    // Asks for what follows the last event that arrived: a stream, or a poll.
    void stream() {
        std::string url;
        std::uint64_t generation = 0;
        std::shared_ptr<http::Request> previous;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!open_ || done_)
                return;
            generation = ++streamGeneration_;
            url = base_ + "/session/" + session_ + "/events?after=" + std::to_string(cursor_) +
                  (poll_ ? "&poll=1" : "");
            ready_ = poll_;  // a poll has nothing to be ready for
            streamStarted_ = Clock::now();
            parser_ = {};
            previous = std::exchange(stream_, nullptr);
        }
        if (previous)
            previous->cancel();

        auto status = std::make_shared<int>(0);
        std::weak_ptr<HttpConnection> weak = shared_from_this();
        http::Handlers handlers;
        handlers.response = [weak, generation, status](int code) {
            *status = code;
            auto self = weak.lock();
            if (!self)
                return;
            if (code == 404)
                self->finish(generation,
                             "the signalling server no longer knows this session: it restarted, "
                             "or gave up on us");
            else if (code != 200)
                self->finish(generation, "the event stream answered " + std::to_string(code));
        };
        handlers.body = [weak, generation](const char* data, std::size_t size) {
            if (auto self = weak.lock())
                self->received(generation, data, size);
        };
        handlers.done = [weak, generation, status](const std::string& error) {
            auto self = weak.lock();
            if (!self)
                return;
            if (!error.empty())
                self->finish(generation, "the event stream failed: " + error);
            else if (*status == 200 && self->current(generation))
                self->stream();  // it ended, as streams here do; the next one
        };
        auto request = http::start("GET", url, "", std::move(handlers));
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation == streamGeneration_ && !done_)
            stream_ = request;
        else if (request)
            request->cancel();
    }

    void received(std::uint64_t generation, const char* data, std::size_t size) {
        std::vector<std::string> frames;
        std::optional<std::string> closed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation != streamGeneration_ || done_)
                return;
            parser_.feed(data, size, [&](const EventParser::Event& event) {
                if (closed)
                    return;
                if (event.type == "ready") {
                    ready_ = true;
                } else if (event.type == "close") {
                    std::string detail = event.data;
                    try {
                        const auto body = nlohmann::json::parse(event.data);
                        detail = std::to_string(body.value("code", 0)) + " " +
                                 body.value("reason", std::string());
                    } catch (...) {
                    }
                    closed = "the signalling server closed the session: " + detail;
                } else if (event.type == "message" && event.hasData) {
                    // Never throws: this runs on the HTTP client's thread.
                    char* end = nullptr;
                    const auto id = std::strtoull(event.id.c_str(), &end, 10);
                    if (!event.id.empty() && end && *end == '\0')
                        cursor_ = id;
                    frames.push_back(event.data);
                }
            });
        }
        // One delivery at a time: a stream let go of mid-delivery must not
        // interleave with the one that replaced it.
        std::lock_guard<std::mutex> delivering(delivery_);
        std::function<void(const std::string&)> message;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation != streamGeneration_ || done_)
                return;
            message = callbacks_.message;
        }
        for (const auto& frame : frames) {
            if (message)
                message(frame);
        }
        if (closed)
            finish(*closed);
    }

    // Sends what has queued up, unless a POST is already on its way.
    void pump() {
        std::string url;
        std::string body;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (posting_ || outbox_.empty() || !open_ || done_)
                return;
            posting_ = true;
            url = base_ + "/session/" + session_;
            for (const auto& frame : outbox_) {
                body += frame;
                body += '\n';
            }
            outbox_.clear();
        }

        auto status = std::make_shared<int>(0);
        std::weak_ptr<HttpConnection> weak = shared_from_this();
        http::Handlers handlers;
        handlers.response = [status](int code) { *status = code; };
        handlers.done = [weak, status](const std::string& error) {
            auto self = weak.lock();
            if (!self)
                return;
            if (!error.empty()) {
                self->finish("sending to the signalling server failed: " + error);
                return;
            }
            if (*status == 404) {
                self->finish("the signalling server no longer knows this session");
                return;
            }
            if (*status < 200 || *status >= 300) {
                self->finish("the signalling server answered " + std::to_string(*status) +
                             " to our frames");
                return;
            }
            {
                std::lock_guard<std::mutex> lock(self->mutex_);
                self->posting_ = false;
            }
            self->pump();
        };
        auto request = http::start("POST", url, body, std::move(handlers));
        std::lock_guard<std::mutex> lock(mutex_);
        if (request && !done_)
            requests_.push_back(request);
        // Finished ones need not be kept.
        while (requests_.size() > 4)
            requests_.pop_front();
    }

    bool current(std::uint64_t generation) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return generation == streamGeneration_ && !done_;
    }

    void finish(std::uint64_t generation, const std::string& why) {
        if (current(generation))
            finish(why);
    }

    void finish(const std::string& why) {
        std::function<void(const std::string&)> closed;
        std::deque<std::shared_ptr<http::Request>> requests;
        std::shared_ptr<http::Request> stream;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (done_)
                return;
            done_ = true;
            closed = std::move(callbacks_.closed);
            callbacks_ = {};
            requests.swap(requests_);
            stream = std::exchange(stream_, nullptr);
        }
        for (auto& request : requests)
            request->cancel();
        if (stream)
            stream->cancel();
        if (closed)
            closed(why);
    }

    mutable std::mutex mutex_;
    std::mutex delivery_;  // held while frames are handed over, never with mutex_
    Callbacks callbacks_;
    std::string base_;
    std::string session_;
    bool open_ = false;
    bool done_ = false;

    std::shared_ptr<http::Request> stream_;
    std::uint64_t streamGeneration_ = 0;
    std::uint64_t cursor_ = 0;
    EventParser parser_;
    bool poll_ = false;
    bool ready_ = false;
    std::optional<Clock::time_point> streamStarted_;

    std::deque<std::string> outbox_;
    bool posting_ = false;
    std::deque<std::shared_ptr<http::Request>> requests_;
};

} // namespace

std::shared_ptr<SignallingConnection> makeHttpConnection(bool poll) {
    if (!http::available())
        return nullptr;
    return std::make_shared<HttpConnection>(poll);
}

} // namespace snps::detail
