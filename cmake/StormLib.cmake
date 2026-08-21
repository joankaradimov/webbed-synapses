# StormLib, fetched on demand, for the caps.mpq builder.
#
# MIT, like this repository, and the reference implementation of the MPQ format
# -- which matters here because the archive has to be read by a 1998 binary we
# cannot debug. It is a build-time dependency of a host tool only: nothing that
# storm loads links against it.
#
# Fetched rather than vendored, in keeping with libdatachannel. Guarded by
# SNP_BUILD_CAPS_TOOLS so that a configure which only wants the layout checks
# does not reach for the network.

include(FetchContent)

set(SNP_STORMLIB_TAG "v9.40" CACHE STRING "StormLib release used by the caps builder")

function(snp_require_stormlib)
  if(TARGET StormLib::storm)
    return()
  endif()

  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(STORM_SKIP_INSTALL ON CACHE BOOL "" FORCE)
  set(STORM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  if(MSVC)
    # StormLib prefers system zlib/bzip2 when it can find them; its bundled
    # copies are what its own MSVC builds use.
    set(CMAKE_DISABLE_FIND_PACKAGE_BZIP2 TRUE PARENT_SCOPE)
    set(CMAKE_DISABLE_FIND_PACKAGE_ZLIB TRUE PARENT_SCOPE)
    set(CMAKE_DISABLE_FIND_PACKAGE_BZIP2 TRUE)
    set(CMAKE_DISABLE_FIND_PACKAGE_ZLIB TRUE)
  endif()

  FetchContent_Declare(stormlib
    GIT_REPOSITORY https://github.com/ladislav-zezula/StormLib.git
    GIT_TAG        ${SNP_STORMLIB_TAG}
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(stormlib)

  # Our /W4 lands on StormLib's bundled zlib and bzip2 otherwise, and hundreds
  # of warnings from a dependency drown the ones from our own code.
  if(MSVC AND TARGET storm)
    target_compile_options(storm PRIVATE /W0)
  endif()
endfunction()
