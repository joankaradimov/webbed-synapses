# Storm, played twice against the real WebRTC module. 32-bit like the module.
add_executable(snp-sim snp_sim.cpp)
target_link_libraries(snp-sim PRIVATE snp_abi)
target_compile_definitions(snp-sim PRIVATE _CRT_SECURE_NO_WARNINGS)
add_dependencies(snp-sim webbed)

if(SNPS_CAN_RUN_SERVER)
  add_test(NAME snp-sim
    COMMAND "${SNPS_NODE}" "${SNPS_RUN_WITH_SERVER}"
            "$<TARGET_FILE:snp-sim>" --module "$<TARGET_FILE_DIR:webbed>/webbed.snp")
  set_tests_properties(snp-sim PROPERTIES TIMEOUT 180)

  # webbed.snp over HTTP, WinHTTP inside a module that unloads: see
  # tests/harness/CMakeLists.txt for the two modes.
  foreach(mode http http-buffered)
    add_test(NAME snp-sim-${mode}
      COMMAND "${SNPS_NODE}" "${SNPS_RUN_WITH_SERVER}" --${mode}
              "$<TARGET_FILE:snp-sim>" --module "$<TARGET_FILE_DIR:webbed>/webbed.snp")
    set_tests_properties(snp-sim-${mode} PROPERTIES TIMEOUT 180)
  endforeach()
endif()
