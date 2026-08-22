# A build experiment, not part of the build.
#
# AGENT.md section 9 step 2 and docs/webrtc-stack.md risk 1: libdatachannel
# supports MSVC, but whether the whole chain -- usrsctp, libjuice, the TLS
# backend -- configures, compiles and *runs* as 32-bit x86 is the highest-value
# unknown in the plan. This project answers it by building the stack the way the
# transport core would consume it and then opening a real peer connection.
#
#   cmake -S tools/webrtc-probe -B build/webrtc-probe -G "Visual Studio 17 2022" -A Win32
#   cmake --build build/webrtc-probe --config RelWithDebInfo
#   build/webrtc-probe/RelWithDebInfo/webrtc_probe.exe
#
# Kept out of the top-level build on purpose. It closed its question
# (docs/webrtc-stack.md) and stays as the reproduction: the stack alone, with
# no transport core in the way. The main build fetches the same stack for the
# core.

cmake_minimum_required(VERSION 3.21)
project(webrtc_probe LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  # Match the main build: modules link the static CRT (see SnpModule.cmake).
  set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

message(STATUS "webrtc-probe: pointer size ${CMAKE_SIZEOF_VOID_P} bytes")

# The same dependency set the main build uses, from the same module, so the
# experiment keeps describing what actually ships.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../cmake")
include(LibDataChannel)
snps_require_libdatachannel()

add_executable(webrtc_probe probe.cpp)
target_link_libraries(webrtc_probe PRIVATE LibDataChannel::LibDataChannelStatic)
