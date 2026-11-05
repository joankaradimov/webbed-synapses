# The core in WebAssembly, built the way DevilutionX's web build builds it.
# Only under Emscripten; tests/CMakeLists.txt adds this directory instead of the
# native tests.
add_executable(snps-wasm-test wasm_test.cpp)
target_link_libraries(snps-wasm-test PRIVATE snps::core)
# The game's own code has no exceptions; the core brings its own
# (core/CMakeLists.txt). This file stands in for the game.
target_compile_options(snps-wasm-test PRIVATE -fno-exceptions)
target_link_options(snps-wasm-test PRIVATE
  -sASYNCIFY
  -sEXIT_RUNTIME=1
  -sENVIRONMENT=web,node
  "SHELL:--pre-js \"${CMAKE_CURRENT_SOURCE_DIR}/node-window.js\"")

# The page the browser half runs in, beside the program. Emscripten builds are
# single-configuration, so that is this directory; copied at configure time
# so an edit to the page alone still arrives.
configure_file(test.html "${CMAKE_CURRENT_BINARY_DIR}/test.html" COPYONLY)

if(SNPS_CAN_RUN_SERVER)
  # The server path, in Node. Data channels need a browser: see README.md.
  # --respawn, because it ends by killing the server and reconnecting to the
  # one that replaces it.
  add_test(NAME wasm-server-path
    COMMAND "${SNPS_NODE}" "${SNPS_RUN_WITH_SERVER}" --respawn 2
            "$<TARGET_FILE:snps-wasm-test>")
  set_tests_properties(wasm-server-path PROPERTIES TIMEOUT 120)

  # Over fetch(), behind a host that will not upgrade, and one that buffers:
  # see tests/harness/CMakeLists.txt.
  foreach(mode http http-buffered)
    add_test(NAME wasm-server-path-${mode}
      COMMAND "${SNPS_NODE}" "${SNPS_RUN_WITH_SERVER}" --respawn 2 --${mode}
              "$<TARGET_FILE:snps-wasm-test>")
    set_tests_properties(wasm-server-path-${mode} PROPERTIES TIMEOUT 120)
  endforeach()
endif()
