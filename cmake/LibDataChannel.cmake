# libdatachannel and what it needs, configured the way docs/webrtc-stack.md
# describes and tools/webrtc-probe proved on 32-bit MSVC.
#
# Call snps_require_libdatachannel() from the top-level CMakeLists, *before*
# any project-wide compile options: FetchContent adds these projects as
# subdirectories, and a subdirectory inherits the directory's options -- our
# /W4 /permissive- has no business inside other people's code.
#
# Provides LibDataChannel::LibDataChannelStatic and nlohmann_json::nlohmann_json.
# Under Emscripten the first is datachannel-wasm, which offers libdatachannel's
# API over the browser's own WebRTC.

include(FetchContent)

set(SNPS_LIBDATACHANNEL_TAG "v0.24.5" CACHE STRING "libdatachannel release")
set(SNPS_DATACHANNEL_WASM_TAG "v0.4.0" CACHE STRING "datachannel-wasm release")

function(snps_require_libdatachannel)
  if(TARGET LibDataChannel::LibDataChannelStatic)
    return()
  endif()

  if(EMSCRIPTEN)
    _snps_require_datachannel_wasm()
    return()
  endif()

  set(BUILD_SHARED_LIBS OFF)

  # --- Mbed TLS ------------------------------------------------------------
  # libdatachannel vendors no TLS backend. With USE_MBEDTLS it runs
  # find_package(MbedTLS 3 REQUIRED) unless MbedTLS::MbedTLS already exists,
  # which is the hook used here. The release tarball needs no submodules.
  FetchContent_Declare(mbedtls
    URL      https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
    URL_HASH SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
  set(ENABLE_PROGRAMS       OFF CACHE BOOL "" FORCE)
  set(ENABLE_TESTING        OFF CACHE BOOL "" FORCE)
  set(MBEDTLS_AS_SUBPROJECT ON  CACHE BOOL "" FORCE)
  # DTLS-SRTP must be compiled in even with NO_MEDIA: libdatachannel's Mbed TLS
  # transport configures it unconditionally. See the header for the citation.
  set(MBEDTLS_USER_CONFIG_FILE
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/mbedtls-user-config.h"
    CACHE FILEPATH "" FORCE)
  FetchContent_MakeAvailable(mbedtls)

  if(NOT TARGET MbedTLS::MbedTLS)
    add_library(MbedTLS::MbedTLS INTERFACE IMPORTED GLOBAL)
    set_target_properties(MbedTLS::MbedTLS PROPERTIES
      INTERFACE_LINK_LIBRARIES "mbedtls;mbedx509;mbedcrypto")
  endif()

  # --- libdatachannel --------------------------------------------------------
  # No media (drops libsrtp), Mbed TLS rather than OpenSSL, WebSockets kept:
  # datachannel-wasm mirrors that API too, so the signalling client is
  # write-once across native and wasm.
  set(NO_MEDIA     ON  CACHE BOOL "" FORCE)
  set(NO_WEBSOCKET OFF CACHE BOOL "" FORCE)
  set(NO_EXAMPLES  ON  CACHE BOOL "" FORCE)
  set(NO_TESTS     ON  CACHE BOOL "" FORCE)
  set(USE_MBEDTLS  ON  CACHE BOOL "" FORCE)

  FetchContent_Declare(libdatachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG        ${SNPS_LIBDATACHANNEL_TAG}
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(libdatachannel)
  # libjuice, which it bundles as juice-static, exports a global udp_sendto,
  # and so does lwIP. A consumer that also links lwIP renames libjuice's.
  # DevilutionX links it through libzt, and does this in its own shim:
  #   target_compile_definitions(juice-static PRIVATE udp_sendto=juice_udp_sendto)
  # docs/webrtc-stack.md, risk 4.

  # --- nlohmann/json -------------------------------------------------------
  # libdatachannel carries it as a submodule for its examples only. Reusing
  # that copy pins it with libdatachannel and costs no second download.
  if(NOT TARGET nlohmann_json::nlohmann_json)
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    set(JSON_Install    OFF CACHE INTERNAL "")
    add_subdirectory("${libdatachannel_SOURCE_DIR}/deps/json"
                     "${libdatachannel_BINARY_DIR}/deps/json" EXCLUDE_FROM_ALL)
  endif()
endfunction()

# The browser has WebRTC built in: ICE, DTLS and SCTP are its own, so there is
# no libjuice, usrsctp or Mbed TLS to build. datachannel-wasm is by
# libdatachannel's author and puts the same API over the browser's
# RTCPeerConnection and WebSocket. Its target carries the JavaScript glue
# (--js-library) as a PUBLIC link option, so it reaches whatever links the core.
function(_snps_require_datachannel_wasm)
  # The patch makes its JavaScript work under a current Emscripten, and keeps
  # a socket closed while connecting out of the browser's console; the script
  # says what it changes and why.
  FetchContent_Declare(datachannel-wasm
    GIT_REPOSITORY https://github.com/paullouisageneau/datachannel-wasm.git
    GIT_TAG        ${SNPS_DATACHANNEL_WASM_TAG}
    GIT_SHALLOW    TRUE
    PATCH_COMMAND  "${CMAKE_COMMAND}" -P
                   "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/datachannel-wasm-patch.cmake")
  FetchContent_MakeAvailable(datachannel-wasm)
  # It throws, and a consumer may compile everything with -fno-exceptions, as
  # DevilutionX does. core/CMakeLists.txt says why these are JavaScript
  # exceptions.
  target_compile_options(datachannel-wasm PRIVATE -fexceptions)
  # Its JavaScript reaches the link as --js-library options, which no build
  # tool counts as inputs: a change to the glue, the patch's included, relinked
  # nothing. Every program that links it depends on the files instead.
  set_property(TARGET datachannel-wasm APPEND PROPERTY INTERFACE_LINK_DEPENDS
    "${datachannel-wasm_SOURCE_DIR}/wasm/js/webrtc.js"
    "${datachannel-wasm_SOURCE_DIR}/wasm/js/websocket.js")
  # Under the native name, so the core links one thing on every target.
  add_library(LibDataChannel::LibDataChannelStatic ALIAS datachannel-wasm)

  # libdatachannel's copy of nlohmann/json is not here, so fetch the same
  # release on its own. The tarball is the library alone, 112 KB.
  if(NOT TARGET nlohmann_json::nlohmann_json)
    FetchContent_Declare(nlohmann_json
      URL      https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
      URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
      DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    set(JSON_Install    OFF CACHE INTERNAL "")
    FetchContent_MakeAvailable(nlohmann_json)
  endif()
endfunction()
