// HTTP in the browser: fetch(), through http_client.js. Single-threaded, so
// every handler runs on the event loop, between the host's own calls.

#include "http_client.hpp"

#include <map>
#include <utility>

extern "C" {
int snps_http_start(const char* method, const char* url, const char* body, int bodyLength,
                    void (*onResponse)(int, void*), void (*onBody)(const char*, int, void*),
                    void (*onDone)(const char*, void*), void* user);
void snps_http_cancel(int id);
}

namespace snps::detail::http {

namespace {

class FetchRequest final : public Request, public std::enable_shared_from_this<FetchRequest> {
public:
    explicit FetchRequest(Handlers handlers) : handlers_(std::move(handlers)) {}

    void begin(const std::string& method, const std::string& url, const std::string& body) {
        // Kept alive by the table below until the request ends: dropping the
        // last outside reference must not stop it.
        id_ = snps_http_start(method.c_str(), url.c_str(), body.data(),
                              static_cast<int>(body.size()), &FetchRequest::onResponse,
                              &FetchRequest::onBody, &FetchRequest::onDone, this);
        live()[id_] = shared_from_this();
    }

    void cancel() override {
        if (!id_)
            return;
        snps_http_cancel(id_);
        end();
    }

    static void cancelAll() {
        auto table = std::move(live());
        live().clear();
        for (auto& entry : table)
            entry.second->cancel();
    }

private:
    static std::map<int, std::shared_ptr<FetchRequest>>& live() {
        static std::map<int, std::shared_ptr<FetchRequest>> table;
        return table;
    }

    void end() {
        handlers_ = {};
        const int id = std::exchange(id_, 0);
        live().erase(id);  // possibly the last reference: touch nothing after
    }

    static void onResponse(int status, void* user) {
        auto* self = static_cast<FetchRequest*>(user);
        if (self->id_ && self->handlers_.response)
            self->handlers_.response(status);
    }

    static void onBody(const char* data, int size, void* user) {
        auto* self = static_cast<FetchRequest*>(user);
        if (self->id_ && self->handlers_.body)
            self->handlers_.body(data, static_cast<std::size_t>(size));
    }

    static void onDone(const char* error, void* user) {
        auto* self = static_cast<FetchRequest*>(user);
        if (!self->id_)
            return;
        auto done = std::move(self->handlers_.done);
        auto keep = self->shared_from_this();  // through the handler, which may cancel()
        self->end();
        if (done)
            done(error ? error : "");
    }

    Handlers handlers_;
    int id_ = 0;
};

} // namespace

bool available() { return true; }

namespace {

// A request nobody is told about: the JavaScript side never calls back, so
// nothing on this side has to outlive it.
class Unwatched final : public Request {
public:
    explicit Unwatched(int id) : id_(id) {}
    void cancel() override { snps_http_cancel(std::exchange(id_, 0)); }

private:
    int id_;
};

} // namespace

std::shared_ptr<Request> start(const std::string& method, const std::string& url,
                               const std::string& body, Handlers handlers) {
    if (!handlers.response && !handlers.body && !handlers.done)
        return std::make_shared<Unwatched>(
            snps_http_start(method.c_str(), url.c_str(), body.data(), static_cast<int>(body.size()),
                            nullptr, nullptr, nullptr, nullptr));
    auto request = std::make_shared<FetchRequest>(std::move(handlers));
    request->begin(method, url, body);
    return request;
}

// Nothing that calls back may still be running once the program has gone:
// its function pointers would point into a runtime that no longer exists.
void shutdown() { FetchRequest::cancelAll(); }

} // namespace snps::detail::http
