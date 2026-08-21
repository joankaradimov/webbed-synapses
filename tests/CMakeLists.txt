# Compiled, never linked: the static_asserts are the test, so a successful
# compile is a pass. This lets the layout be checked on any host with a 32-bit
# capable compiler, no Windows and no linker required.
if(NOT EMSCRIPTEN)
  add_library(snp_abi_layout OBJECT abi_layout.cpp)
  target_link_libraries(snp_abi_layout PRIVATE snp_abi)
endif()

if(NOT SNPS_BUILD_TRANSPORT)
  return()
endif()

# The transport tests need a signalling server: server/, which
# tests/harness/run.mjs starts and then runs the test against. All of it is
# Node: the server, the drivers, and a test that is itself JavaScript all run
# under the one found here, whatever ctest's own PATH.
find_program(SNPS_NODE NAMES node DOC "Node.js, for the signalling server and the test drivers")
if(NOT SNPS_NODE AND CMAKE_CROSSCOMPILING_EMULATOR)
  set(SNPS_NODE "${CMAKE_CROSSCOMPILING_EMULATOR}")  # Emscripten's is Node
endif()
set(SNPS_RUN_WITH_SERVER "${CMAKE_CURRENT_SOURCE_DIR}/harness/run.mjs")
if(SNPS_NODE)
  set(SNPS_CAN_RUN_SERVER TRUE)
  if(NOT EXISTS "${PROJECT_SOURCE_DIR}/server/node_modules/ws")
    message(WARNING "The transport tests start server/, which needs `npm ci` there first.")
  endif()
else()
  set(SNPS_CAN_RUN_SERVER FALSE)
  message(STATUS "snps: transport tests skipped; they need Node")
endif()

# In the browser build, the core's own test, the way DevilutionX's web build
# uses it, and nothing native.
if(EMSCRIPTEN)
  add_subdirectory(wasm)
  return()
endif()

add_subdirectory(harness)

if(SNP_BUILD_PROVIDERS AND WIN32 AND TARGET webbed)
  add_subdirectory(snp-sim)
endif()
