# The transport core. Static, built from source in whatever tree consumes it.
add_library(snps_core STATIC
  src/version.cpp)

add_library(snps::core ALIAS snps_core)

target_include_directories(snps_core PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}/include")

target_compile_definitions(snps_core PRIVATE
  SNPS_CORE_VERSION="${PROJECT_VERSION}")

# C++17, and no higher: this has to build under 32-bit MSVC and inside
# DevilutionX. Not a minimum -- a ceiling on what may be used.
target_compile_features(snps_core PUBLIC cxx_std_17)

set_target_properties(snps_core PROPERTIES
  POSITION_INDEPENDENT_CODE ON)

# The WebRTC transport itself. Off in the layout-only build, which has no use
# for libdatachannel and no business fetching it.
if(SNPS_BUILD_TRANSPORT)
  target_sources(snps_core PRIVATE
    src/transport.cpp
    src/signalling_ws.cpp
    src/signalling_http.cpp)

  # The HTTP client under the signalling connection's HTTP form, for servers
  # behind a host that will not upgrade to a WebSocket. One per platform; the
  # rest have none yet, and reach WebSocket servers only (src/http_client.hpp
  # says what a new one takes).
  if(EMSCRIPTEN)
    target_sources(snps_core PRIVATE src/http_client_wasm.cpp)
    target_link_options(snps_core INTERFACE
      "SHELL:--js-library \"${CMAKE_CURRENT_LIST_DIR}/src/http_client.js\"")
    # A --js-library is no build tool's input; see cmake/LibDataChannel.cmake.
    set_property(TARGET snps_core APPEND PROPERTY INTERFACE_LINK_DEPENDS
      "${CMAKE_CURRENT_LIST_DIR}/src/http_client.js")
  elseif(WIN32)
    target_sources(snps_core PRIVATE src/http_client_winhttp.cpp)
    target_link_libraries(snps_core PRIVATE winhttp)
  else()
    target_sources(snps_core PRIVATE src/http_client_none.cpp)
  endif()
  # PRIVATE: no libdatachannel type appears in snps/transport.hpp, so hosts
  # compile against the core without the WebRTC headers. Static libraries
  # still propagate the link dependency.
  target_link_libraries(snps_core PRIVATE
    LibDataChannel::LibDataChannelStatic
    nlohmann_json::nlohmann_json)
  target_compile_definitions(snps_core PUBLIC SNPS_HAS_TRANSPORT=1)

  # In the browser: the core, datachannel-wasm and nlohmann/json all catch
  # exceptions, while a consumer may build everything else with
  # -fno-exceptions (DevilutionX does). -fexceptions after it turns them back
  # on for this code alone. They are JavaScript exceptions because those work
  # with -sASYNCIFY; -fwasm-exceptions does not (emcc warns that the two are
  # incompatible, and a sleep inside a catch block breaks). The link option
  # reaches the consumer's final link, which has to agree.
  if(EMSCRIPTEN)
    target_compile_options(snps_core PRIVATE -fexceptions)
    target_link_options(snps_core INTERFACE -fexceptions)
  endif()
endif()
