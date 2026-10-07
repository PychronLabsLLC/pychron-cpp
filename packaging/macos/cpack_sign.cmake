# CPack pre-build script (CPACK_PRE_BUILD_SCRIPTS, cmake/PychronPackaging.cmake):
# signs Pychron.app in the staging directory, after it is installed, stripped
# and has its Qt runtime, and before the disk image is made from it.
#
# The identity is the PYCHRON_CODESIGN_IDENTITY environment variable: a
# "Developer ID Application: ..." certificate, or "-" for ad hoc. Unset, the
# app is left as the install made it (a local build that is not distributed).

set(_identity "$ENV{PYCHRON_CODESIGN_IDENTITY}")
if(_identity STREQUAL "")
  message(STATUS "pychron: PYCHRON_CODESIGN_IDENTITY is not set; Pychron.app is not signed")
  return()
endif()

file(GLOB_RECURSE _apps LIST_DIRECTORIES true "${CPACK_TEMPORARY_DIRECTORY}/*")
list(FILTER _apps INCLUDE REGEX "/Pychron\\.app$")
list(LENGTH _apps _n)
if(NOT _n EQUAL 1)
  message(FATAL_ERROR "pychron: expected one Pychron.app under ${CPACK_TEMPORARY_DIRECTORY}, found: ${_apps}")
endif()

execute_process(
  COMMAND "${CPACK_PYCHRON_SIGN_SCRIPT}" "${_apps}" "${_identity}" "${CPACK_PYCHRON_ENTITLEMENTS}"
  RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "pychron: signing ${_apps} failed (${_rc})")
endif()
