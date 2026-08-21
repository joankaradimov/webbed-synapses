# Script mode. Packages a built module as a .snp:
#
#     cmake -DMODULE=<dll> -DARCHIVE=<caps.mpq> -DOUT=<module.snp> -P SnpPackage.cmake
#
# ARCHIVE may be empty, which produces a bare copy that storm will not
# enumerate -- see add_snp_module in SnpModule.cmake and docs/snp-abi.md
# section 2.
#
# This exists because there is no `cmake -E` command that concatenates into a
# file: `cat` writes to stdout, and add_custom_command runs no shell, so a `>`
# in one is passed to cmake as a literal argument rather than redirecting.
# execute_process(OUTPUT_FILE) does the redirection, byte for byte.

if(NOT MODULE OR NOT OUT)
  message(FATAL_ERROR "SnpPackage.cmake: MODULE and OUT are required")
endif()

if(ARCHIVE)
  if(NOT EXISTS "${ARCHIVE}")
    message(FATAL_ERROR "SNP_CAPS_ARCHIVE does not exist: ${ARCHIVE}")
  endif()

  # The archive has to begin on a 512-byte boundary. Storm looks for the MPQ
  # signature at offset 0 and steps forward 512 bytes at a time until it finds
  # it or runs out of file -- SFILE.CPP:2104 [S], and the same granularity is
  # why appending an archive to a DLL works at all. A PE built with the default
  # FileAlignment is already a multiple of 512, so this padding is usually
  # empty; when it is not, the alternative is a module storm skips in silence.
  file(SIZE "${MODULE}" module_size)
  math(EXPR padding "(512 - ${module_size} % 512) % 512")
  set(parts "${MODULE}")
  if(padding)
    message(STATUS "SnpPackage: ${module_size} bytes, padding ${padding} to a "
                   "512-byte boundary before the archive")
    # Dead space between the module and the archive: nothing reads it, and the
    # filler only has to avoid being an MPQ signature itself.
    string(REPEAT "0" ${padding} filler)
    file(WRITE "${OUT}.pad" "${filler}")
    list(APPEND parts "${OUT}.pad")
  endif()
  list(APPEND parts "${ARCHIVE}")

  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E cat ${parts}
    OUTPUT_FILE "${OUT}"
    RESULT_VARIABLE result)
  file(REMOVE "${OUT}.pad")
  if(result)
    message(FATAL_ERROR "packaging ${OUT} failed: ${result}")
  endif()
else()
  file(COPY_FILE "${MODULE}" "${OUT}" ONLY_IF_DIFFERENT)
endif()
