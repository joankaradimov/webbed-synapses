#ifndef SNPS_SIGNALLING_HPP
#define SNPS_SIGNALLING_HPP

// One connection to the signalling server: JSON text frames, both ways
// (docs/signalling-protocol.md). The transport holds one at a time, and opens
// a new one for every reconnect. Two kinds: a WebSocket, and HTTP -- the
// server pushing by Server-Sent Events, frames POSTed -- for hosts whose
// proxies will not upgrade a connection.
//
// Internal to the core. Callbacks run on whatever thread the connection's
// machinery uses -- a libdatachannel thread, an HTTP worker, the browser's
// event loop -- and never while a call into the connection is still holding
// a lock of its own.

#include <functional>
#include <memory>
#include <string>

namespace snps::detail {

class SignallingConnection {
public:
    struct Callbacks {
        std::function<void()> opened;                    // ready to send
        std::function<void(const std::string&)> message; // one frame
        std::function<void(const std::string&)> closed;  // why; at most once, and final
        std::function<void(const std::string&)> note;    // worth a line in the log
        std::function<void()> buffered;  // HTTP: something in between holds streams back
    };

    virtual ~SignallingConnection() = default;

    virtual void open(const std::string& url, Callbacks callbacks) = 0;
    virtual bool isOpen() const = 0;
    // Never blocks. Frames arrive at the server in the order they were sent.
    virtual void send(const std::string& text) = 0;
    // Nothing is reported after this, closed() included.
    virtual void close() = 0;
    // About once a second, from the transport's tick, for anything the
    // connection has to time itself.
    virtual void tick() {}
};

std::shared_ptr<SignallingConnection> makeWebSocketConnection();

// Null where this build has no HTTP client (http_client.hpp says which do).
// `poll`: long-poll from the start, because an earlier connection found its
// streams held back.
std::shared_ptr<SignallingConnection> makeHttpConnection(bool poll);

} // namespace snps::detail

#endif // SNPS_SIGNALLING_HPP
