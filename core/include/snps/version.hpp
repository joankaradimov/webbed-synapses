#ifndef SNPS_VERSION_HPP
#define SNPS_VERSION_HPP

namespace snps {

// Identifies the transport core build, for logs and diagnostics. Nothing sends
// it to peers yet; a handshake that did would make a version mismatch a clear
// error rather than a hang.
const char* CoreVersion() noexcept;

} // namespace snps

#endif // SNPS_VERSION_HPP
