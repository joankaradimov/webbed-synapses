// HTTP over WinHTTP, the client Windows ships: it verifies certificates
// against the system's store and follows the system's proxy settings, which
// is what a game on a player's machine should do.
//
// Synchronous WinHTTP, one worker thread per request. A signalling session
// has at most three in flight -- its event stream, a POST, a goodbye -- so
// this is the simple way, not the scalable one. Cancelling closes the request
// handle from another thread, which WinHTTP allows and which ends whatever
// call the worker is blocked in.
//
// Every worker is joined, never detached: by the next start() once it has
// finished, or by shutdown(). A thread still running when webbed.snp unloads
// would run on in code that is no longer there.

#include "http_client.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <list>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace snps::detail::http {

namespace {

// Name resolution, connecting, sending: a server that takes longer is down.
constexpr int kSetupTimeoutMs = 10'000;
// Receiving: an event stream says something every ten seconds and ends
// within twenty-five (server/src/http.js), so silence this long means lost.
constexpr int kReceiveTimeoutMs = 40'000;

std::wstring widen(const std::string& text) {
    if (text.empty())
        return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::string describe(DWORD code) {
    switch (code) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "the server's name does not resolve";
    case ERROR_WINHTTP_CANNOT_CONNECT: return "cannot connect to the server";
    case ERROR_WINHTTP_CONNECTION_ERROR: return "the connection was reset";
    case ERROR_WINHTTP_TIMEOUT: return "timed out";
    case ERROR_WINHTTP_SECURE_FAILURE: return "the TLS handshake failed";
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID: return "the certificate is for another name";
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID: return "the certificate has expired";
    case ERROR_WINHTTP_SECURE_INVALID_CA: return "the certificate is not from a trusted authority";
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME: return "the url is not http:// or https://";
    case ERROR_WINHTTP_INVALID_URL: return "the url is not valid";
    default: return "WinHTTP error " + std::to_string(code);
    }
}

// One process-wide session handle, and every worker ever started.
struct Client {
    std::mutex mutex;
    HINTERNET session = nullptr;
    struct Worker {
        std::thread thread;
        bool finished = false;
    };
    std::list<Worker> workers;
    std::vector<std::weak_ptr<Request>> live;

    HINTERNET open() {
        if (session)
            return session;
        // Automatic proxy discovery needs Windows 8.1; older ones get the
        // configured default.
        session = WinHttpOpen(L"snps", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session)
            session = WinHttpOpen(L"snps", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session)
            WinHttpSetTimeouts(session, kSetupTimeoutMs, kSetupTimeoutMs, kSetupTimeoutMs,
                               kReceiveTimeoutMs);
        return session;
    }

    // Joins the workers that have finished. Call with mutex held.
    void reap() {
        live.erase(std::remove_if(live.begin(), live.end(),
                                  [](const std::weak_ptr<Request>& r) { return r.expired(); }),
                   live.end());
        for (auto it = workers.begin(); it != workers.end();) {
            if (it->finished) {
                if (it->thread.joinable())
                    it->thread.join();
                it = workers.erase(it);
            } else {
                ++it;
            }
        }
    }
};

Client& client() {
    static Client instance;  // a mutex and empty containers: nothing runs at load
    return instance;
}

class WinHttpRequest final : public Request {
public:
    explicit WinHttpRequest(Handlers handlers) : handlers_(std::move(handlers)) {}

    void cancel() override {
        HINTERNET handle = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cancelled_)
                return;
            cancelled_ = true;
            handle = std::exchange(handle_, nullptr);
        }
        if (handle)
            WinHttpCloseHandle(handle);  // ends the worker's blocking call
    }

    // A request that could not start: reported from its worker like any
    // other failure.
    void fail(const std::string& error) { finish(error); }

    // The worker's body.
    void run(HINTERNET session, const std::string& method, const std::string& url,
             const std::string& body) {
        std::wstring wideUrl = widen(url);
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwHostNameLength = static_cast<DWORD>(-1);
        parts.dwUrlPathLength = static_cast<DWORD>(-1);
        parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) {
            finish(describe(GetLastError()));
            return;
        }
        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        if (path.empty())
            path = L"/";

        HINTERNET connection = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
        if (!connection) {
            finish(describe(GetLastError()));
            return;
        }
        HINTERNET request = WinHttpOpenRequest(
            connection, widen(method).c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_REFRESH | (parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
        if (!request) {
            const DWORD code = GetLastError();
            WinHttpCloseHandle(connection);
            finish(describe(code));
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cancelled_) {
                WinHttpCloseHandle(request);
                WinHttpCloseHandle(connection);
                return;
            }
            handle_ = request;
        }

        std::string error = exchange(request, method, body);

        // Closed here, unless cancel() got to it first.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (handle_)
                WinHttpCloseHandle(std::exchange(handle_, nullptr));
        }
        WinHttpCloseHandle(connection);
        finish(error);
    }

private:
    // Sends the request and reads the response through to its end. The error,
    // or empty.
    std::string exchange(HINTERNET request, const std::string& method, const std::string& body) {
        const wchar_t* headers = method == "GET" ? WINHTTP_NO_ADDITIONAL_HEADERS
                                                 : L"Content-Type: text/plain; charset=utf-8\r\n";
        const DWORD size = static_cast<DWORD>(body.size());
        if (!WinHttpSendRequest(request, headers, static_cast<DWORD>(-1),
                                size ? const_cast<char*>(body.data()) : WINHTTP_NO_REQUEST_DATA,
                                size, size, 0) ||
            !WinHttpReceiveResponse(request, nullptr))
            return describe(GetLastError());

        DWORD status = 0;
        DWORD length = sizeof(status);
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &length,
                                 WINHTTP_NO_HEADER_INDEX))
            return describe(GetLastError());
        if (!call([&] { if (handlers_.response) handlers_.response(static_cast<int>(status)); }))
            return {};

        std::vector<char> buffer;
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available))
                return describe(GetLastError());
            if (available == 0)
                return {};  // the end of the body
            buffer.resize(std::max<std::size_t>(buffer.size(), available));
            DWORD read = 0;
            if (!WinHttpReadData(request, buffer.data(), available, &read))
                return describe(GetLastError());
            if (read == 0)
                return {};
            if (!call([&] { if (handlers_.body) handlers_.body(buffer.data(), read); }))
                return {};
        }
    }

    // Runs a handler unless cancelled. False if cancelled.
    template <typename F>
    bool call(F&& f) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cancelled_)
                return false;
        }
        f();
        return true;
    }

    void finish(const std::string& error) {
        call([&] { if (handlers_.done) handlers_.done(error); });
        handlers_ = {};  // whatever they hold goes now, on this thread
    }

    Handlers handlers_;
    std::mutex mutex_;
    bool cancelled_ = false;
    HINTERNET handle_ = nullptr;
};

} // namespace

bool available() { return true; }

std::shared_ptr<Request> start(const std::string& method, const std::string& url,
                               const std::string& body, Handlers handlers) {
    auto request = std::make_shared<WinHttpRequest>(std::move(handlers));
    auto& c = client();
    std::lock_guard<std::mutex> lock(c.mutex);
    c.reap();
    HINTERNET session = c.open();
    if (!session) {
        // Reported from a worker like any other failure, never from inside
        // start(): a caller holding its own lock must not be called back.
        const std::string error = "cannot start WinHTTP: " + describe(GetLastError());
        c.workers.emplace_back();
        auto worker = std::prev(c.workers.end());
        worker->thread = std::thread([request, error, worker] {
            request->fail(error);
            std::lock_guard<std::mutex> lock(client().mutex);
            worker->finished = true;
        });
        return request;
    }
    c.live.push_back(request);
    c.workers.emplace_back();
    auto worker = std::prev(c.workers.end());
    worker->thread = std::thread([request, session, method, url, body, worker] {
        request->run(session, method, url, body);
        std::lock_guard<std::mutex> lock(client().mutex);
        worker->finished = true;
    });
    return request;
}

void shutdown() {
    auto& c = client();
    std::vector<std::shared_ptr<Request>> live;
    {
        std::lock_guard<std::mutex> lock(c.mutex);
        for (auto& weak : c.live)
            if (auto request = weak.lock())
                live.push_back(std::move(request));
        c.live.clear();
    }
    for (auto& request : live)
        request->cancel();
    live.clear();

    std::list<Client::Worker> workers;
    HINTERNET session = nullptr;
    {
        std::lock_guard<std::mutex> lock(c.mutex);
        workers.splice(workers.end(), c.workers);
        session = std::exchange(c.session, nullptr);
    }
    for (auto& worker : workers)
        if (worker.thread.joinable())
            worker.thread.join();
    if (session)
        WinHttpCloseHandle(session);
}

} // namespace snps::detail::http
