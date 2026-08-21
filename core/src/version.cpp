#include "snps/version.hpp"

#if __cplusplus < 201703L && !defined(_MSC_VER)
#error "The transport core requires C++17. See core/README.md."
#endif

namespace snps {

const char* CoreVersion() noexcept
{
    return SNPS_CORE_VERSION;
}

} // namespace snps
