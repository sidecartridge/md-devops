# Writes build_id.h with RELEASE_BUILD_ID, the git commit the firmware was
# built from. Run on every build (not only at configure time), so an
# incremental build never reports a stale ID.
#
#   <sha7>                     clean tree
#   <sha7>-dirty.<diff7>       uncommitted changes; <diff7> hashes the diff,
#                              so different changes give different IDs and the
#                              same tree always gives the same ID
#   $ENV{RELEASE_BUILD_ID}     when set (for example a source copy outside git);
#                              cut to 21 characters
#   nogit                      not a git checkout
#
# Inputs: -DSRC_DIR=<rp/src> -DOUT=<path of build_id.h>

if(DEFINED ENV{RELEASE_BUILD_ID} AND NOT "$ENV{RELEASE_BUILD_ID}" STREQUAL "")
  set(BUILD_ID "$ENV{RELEASE_BUILD_ID}")
else()
  execute_process(
    COMMAND git rev-parse --short=7 HEAD
    WORKING_DIRECTORY "${SRC_DIR}"
    OUTPUT_VARIABLE SHA
    RESULT_VARIABLE SHA_RESULT
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT SHA_RESULT EQUAL 0)
    set(BUILD_ID "nogit")
  else()
    # Submodules are excluded: rp/build.sh checks them out at pinned tags.
    execute_process(
      COMMAND git diff HEAD --binary --ignore-submodules
      WORKING_DIRECTORY "${SRC_DIR}/../.."
      OUTPUT_VARIABLE DIFF
      ERROR_QUIET)
    if("${DIFF}" STREQUAL "")
      set(BUILD_ID "${SHA}")
    else()
      string(SHA1 DIFF_HASH "${DIFF}")
      string(SUBSTRING "${DIFF_HASH}" 0 7 DIFF_HASH)
      set(BUILD_ID "${SHA}-dirty.${DIFF_HASH}")
    endif()
  endif()
endif()

# The health report sizes its buffer for 21 characters (<sha7>-dirty.<diff7>).
string(SUBSTRING "${BUILD_ID}" 0 21 BUILD_ID)

set(CONTENT "#pragma once\n#define RELEASE_BUILD_ID \"${BUILD_ID}\"\n")
if(EXISTS "${OUT}")
  file(READ "${OUT}" OLD)
else()
  set(OLD "")
endif()
if(NOT "${OLD}" STREQUAL "${CONTENT}")
  file(WRITE "${OUT}" "${CONTENT}")
  message(STATUS "RELEASE_BUILD_ID: ${BUILD_ID}")
endif()
