# snp-mpq: the archive packer. A host tool -- it runs during the build and
# nothing storm loads links against it. StormLib is fetched by the top-level
# CMakeLists (cmake/StormLib.cmake).

add_executable(snp-mpq main.cpp)
target_link_libraries(snp-mpq PRIVATE StormLib::storm)
target_compile_features(snp-mpq PRIVATE cxx_std_17)   # std::filesystem

if(MSVC)
  # StormLib's headers use the CRT functions MSVC deprecates.
  target_compile_definitions(snp-mpq PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
