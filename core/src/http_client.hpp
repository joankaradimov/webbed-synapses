#ifndef SNPS_HTTP_CLIENT_HPP
#define SNPS_HTTP_CLIENT_HPP

// The least an HTTP client has to do for the signalling connection over HTTP
// (signalling_http.cpp): one request, with its response's body handed over as
// it arrives. That is all a new platform has to provide to reach a server
// that will not upgrade to a WebSocket; the protocol above it is shared.
//
// Provided by:
//   Windows      http_client_winhttp.cpp  WinHTTP, the system's own: its
//                                         certificate store, its proxy settings
//   Emscripten   http_client_wasm.cpp     the browser's fetch()
//   elsewhere    http_client_none.cpp     nothing; available() is false, and a
//                                         transport told to use HTTP goes Offline
//
// Linux and macOS are the obvious next ones, for DevilutionX's other desktop
// builds: an HTTP/1.1 client over a socket and the Mbed TLS already linked, or
// libcurl. Either goes in a file of its own beside these.
//
// Internal to the core.

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace snps::detail::http {

struct Handlers {
    std::function<void(int status)> response;                     // once, before any body
    std::function<void(const char* data, std::size_t size)> body;  // as it arrives
    std::function<void(const std::string& error)> done;            // last; empty if it all arrived
};

class Request {
public:
    virtual ~Request() = default;
    // Stops it, and never blocks. A handler already running may finish; none
    // starts afterwards. Dropping the last reference does not cancel: a
    // request runs to its end unless told otherwise.
    virtual void cancel() = 0;
};

bool available();

// Starts a request and returns at once. A body is sent as text/plain, which
// keeps a browser on another origin from asking first (CORS). Handlers run on
// the client's own thread natively, the event loop in a browser. Null where
// available() is false.
std::shared_ptr<Request> start(const std::string& method, const std::string& url,
                               const std::string& body, Handlers handlers);

// Waits for every request to end, cancelling them first. snps::shutdown()
// calls it, so that nothing of ours is running when a module unloads.
void shutdown();

} // namespace snps::detail::http

#endif // SNPS_HTTP_CLIENT_HPP
