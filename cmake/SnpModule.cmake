# add_snp_module(<name> SOURCES <...> [DEF <file>] [CAPS <header>])
#
# Builds a provider module and packages it as a .snp.
#
# A .snp is not merely a renamed DLL: storm opens the module file as an MPQ
# archive and reads caps.dat out of it before it will even call SnpQuery
# (SNET.CPP:531-537). A module without an appended archive is skipped during
# enumeration and never reaches the network-selection menu. SnpPackage.cmake
# performs the same step Blizzard's and BWAPI's builds do:
#
#     copy /B module.dll + caps.mpq module.snp
#
# We do not ship a caps.mpq — it is someone else's artifact with unclear
# provenance, and this repository is MIT. Point SNP_CAPS_ARCHIVE at one to get
# a loadable module. See docs/snp-abi.md section 2.

# Where this module lives, so the packaging script can be found from inside a
# function that may be called from any directory.
set(_SNP_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}")

set(SNP_CAPS_ARCHIVE "" CACHE FILEPATH
  "MPQ archive containing caps.dat, appended to each module to make it loadable")

# On by default because storm skips a module whose appended archive is unsigned
# (SNET.CPP:731-737; 1.16.1's storm.dll does the same, docs/snp-abi.md section
# 2). StarCraft lists one carrying a weak signature (2026-09-16).
option(SNP_CAPS_SIGN
  "Give the generated caps archive Blizzard's weak signature" ON)

function(add_snp_module name)
  cmake_parse_arguments(ARG "" "DEF;CAPS" "SOURCES" ${ARGN})

  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "add_snp_module(${name}): SOURCES is required")
  endif()

  add_library(${name} MODULE ${ARG_SOURCES})
  target_link_libraries(${name} PRIVATE snp_abi)
  set_target_properties(${name} PROPERTIES PREFIX "" OUTPUT_NAME "${name}")

  # Static CRT. A module is dropped into a game directory and loaded by a
  # process we do not control; a dependency on VCRUNTIME140.dll that the
  # machine happens not to have makes LoadLibrary fail, and storm reports a
  # failed LoadLibrary exactly the way it reports everything else -- by
  # skipping the module without a word (SNET.CPP:537-541).
  if(MSVC)
    set_property(TARGET ${name} PROPERTY
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()

  # Exports are resolved by name (SNET.CPP:544), so the .def carries no
  # ordinals. It also stops MinGW from decorating __stdcall names.
  if(ARG_DEF)
    if(MSVC)
      target_link_options(${name} PRIVATE "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/${ARG_DEF}")
    else()
      target_sources(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/${ARG_DEF}")
      target_link_options(${name} PRIVATE -Wl,--kill-at)
    endif()
    set_property(TARGET ${name} APPEND PROPERTY
      LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${ARG_DEF}")
  endif()

  # The caps archive: generated from the module's own declaration when the
  # tools are available, overridden by SNP_CAPS_ARCHIVE when someone supplies a
  # prebuilt one, and absent otherwise -- in which case storm will not
  # enumerate the module and the warning below says so.
  set(_archive "${SNP_CAPS_ARCHIVE}")
  if(NOT _archive AND ARG_CAPS AND TARGET snp-mpq)
    set(_staging "${CMAKE_CURRENT_BINARY_DIR}/caps")
    set(_archive "${CMAKE_CURRENT_BINARY_DIR}/${name}-caps.mpq")

    # One generator per module: the same header the module compiles, so the
    # provider records and SnpQuery cannot drift apart (snp/caps_record.h).
    add_executable(${name}-caps "${_SNP_MODULE_DIR}/../tools/caps-gen/main.cpp")
    target_link_libraries(${name}-caps PRIVATE snp_abi)
    target_compile_definitions(${name}-caps PRIVATE
      "SNP_CAPS_TABLE_HEADER=\"${CMAKE_CURRENT_SOURCE_DIR}/${ARG_CAPS}\"")
    if(MSVC)
      target_compile_definitions(${name}-caps PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()

    add_custom_command(
      OUTPUT "${_staging}/caps.dat"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${_staging}"
      COMMAND "$<TARGET_FILE:${name}-caps>" "${_staging}/caps.dat"
      DEPENDS ${name}-caps "${CMAKE_CURRENT_SOURCE_DIR}/${ARG_CAPS}"
      VERBATIM
      COMMENT "Writing ${name} caps.dat from ${ARG_CAPS}")

    # See SNP_CAPS_SIGN above: a verified weak signature is known to be
    # accepted by the game.
    set(_sign "")
    if(SNP_CAPS_SIGN)
      set(_sign "--sign")
    endif()

    add_custom_command(
      OUTPUT "${_archive}"
      COMMAND "$<TARGET_FILE:snp-mpq>" ${_sign} "${_staging}" "${_archive}"
      DEPENDS snp-mpq "${_staging}/caps.dat"
      VERBATIM
      COMMENT "Packing ${name}-caps.mpq")

    add_custom_target(${name}-caps-archive DEPENDS "${_archive}")
    add_dependencies(${name} ${name}-caps-archive)
  endif()

  # Beside the module, whatever the generator's layout is. No BYPRODUCTS: the
  # byproducts of a POST_BUILD command are evaluated without a target context,
  # so a $<TARGET_FILE_DIR:...> in one is a generate-time error under both the
  # Visual Studio and Ninja generators. Nothing in the build consumes the .snp
  # anyway -- it is the final artifact, copied into a game directory by hand.
  set(_snp "$<TARGET_FILE_DIR:${name}>/${name}.snp")

  if(_archive)
    set(_what "Packaging ${name}.snp (module + caps archive)")
  else()
    set(_what "Packaging ${name}.snp WITHOUT a caps archive - storm will not enumerate it")
    if(NOT SNP_CAPS_WARNED)
      message(WARNING
        "No caps archive for ${name}: SNP_BUILD_CAPS_TOOLS is off or the module "
        "declares no CAPS header, and SNP_CAPS_ARCHIVE is unset. It will build, "
        "but storm skips any .snp that is not also a readable MPQ containing "
        "caps.dat, so it will not appear in the game's network menu. See "
        "docs/snp-abi.md section 2.")
      set(SNP_CAPS_WARNED ON CACHE INTERNAL "")
    endif()
  endif()

  add_custom_command(TARGET ${name} POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
            "-DMODULE=$<TARGET_FILE:${name}>"
            "-DARCHIVE=${_archive}"
            "-DOUT=${_snp}"
            -P "${_SNP_MODULE_DIR}/SnpPackage.cmake"
    VERBATIM
    COMMENT "${_what}")
endfunction()
