# snp-load: LoadLibrary a module and put it through storm's enumeration
# sequence. 32-bit like everything else that touches a .snp, which it is
# automatically -- this only builds inside a build that produces modules.

add_executable(snp-load main.cpp)
target_link_libraries(snp-load PRIVATE snp_abi)
if(MSVC)
  target_compile_definitions(snp-load PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
