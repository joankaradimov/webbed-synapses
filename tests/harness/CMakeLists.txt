# Peers, no game: real transports through a real signalling server.
add_executable(snps-harness harness.cpp)
target_link_libraries(snps-harness PRIVATE snps::core)
if(MSVC)
  target_compile_definitions(snps-harness PRIVATE _CRT_SECURE_NO_WARNINGS)  # getenv
endif()

# A peer that answers everything, for testing another build of the core
# against this one: tests/wasm runs the browser's against it.
add_executable(snps-echo echo.cpp)
target_link_libraries(snps-echo PRIVATE snps::core)

if(SNPS_CAN_RUN_SERVER)
  # --respawn: the harness kills the server last, and a new one takes its
  # place two seconds later, for the transports to reconnect to.
  add_test(NAME transport-harness
    COMMAND "${SNPS_NODE}" "${SNPS_RUN_WITH_SERVER}" --respawn 2
            "$<TARGET_FILE:snps-harness>")
  set_tests_properties(transport-harness PROPERTIES TIMEOUT 180)
endif()
