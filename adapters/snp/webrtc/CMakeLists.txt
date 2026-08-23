# The WebRTC provider: storm's function table over snps::core. It ships as
# webbed.snp, named for the project; this directory is named for its transport.
add_snp_module(webbed
  SOURCES webrtc_provider.cpp
  DEF     webrtc.def
  CAPS    webrtc_caps.h)

target_link_libraries(webbed PRIVATE snps::core)
target_compile_definitions(webbed PRIVATE _CRT_SECURE_NO_WARNINGS)
