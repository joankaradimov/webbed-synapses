# The StarCraft side: storm.dll's function table, mapped onto snps::core.
#
# snp_abi is the interface header alone, no code. It is defined unconditionally
# because the layout tests consume it on hosts that build no module at all.
add_library(snp_abi INTERFACE)
target_include_directories(snp_abi INTERFACE
  "${CMAKE_CURRENT_LIST_DIR}/include")

if(NOT SNP_BUILD_PROVIDERS)
  return()
endif()

if(NOT WIN32)
  message(STATUS "snps: not a Windows target, skipping SNP modules")
  return()
endif()

# A .snp is loaded by a 32-bit process. There is no 64-bit variant of this
# problem, so fail loudly rather than producing an artifact nothing can load.
# This constrains the SNP adapter only -- the core also builds x64 and wasm.
if(CMAKE_SIZEOF_VOID_P AND NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR
    "The SNP adapter targets 32-bit x86 only (pointer size is ${CMAKE_SIZEOF_VOID_P} bytes).\n"
    "Use a 32-bit toolchain -- -A Win32 with Visual Studio, or\n"
    "-DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/i686-w64-mingw32.cmake --\n"
    "or configure with -DSNP_BUILD_PROVIDERS=OFF to build only the core.")
endif()

add_subdirectory(null)

# The real one. Needs the transport, which the layout-only build leaves out.
if(SNPS_BUILD_TRANSPORT)
  add_subdirectory(webrtc)
endif()
