// No HTTP client: where the core has none yet, see http_client.hpp. The
// signalling server is reachable from here as a WebSocket only.

#include "http_client.hpp"

namespace snps::detail::http {

bool available() { return false; }

std::shared_ptr<Request> start(const std::string&, const std::string&, const std::string&,
                               Handlers) {
    return nullptr;
}

void shutdown() {}

} // namespace snps::detail::http
