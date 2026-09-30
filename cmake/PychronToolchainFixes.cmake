# Some macOS Command Line Tools installs leave a stale, partial
# <CLT>/usr/include/c++/v1 directory that shadows the SDK's complete libc++
# headers (symptom: "'chrono' file not found"). Detect that and point the
# compiler at the SDK's libc++ headers instead.
if(APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  include(CheckIncludeFileCXX)
  check_include_file_cxx(version PYCHRON_HAVE_STD_VERSION_HEADER)
  if(NOT PYCHRON_HAVE_STD_VERSION_HEADER)
    set(_pychron_sdk "${CMAKE_OSX_SYSROOT}")
    if(NOT _pychron_sdk)
      execute_process(COMMAND xcrun --show-sdk-path
                      OUTPUT_VARIABLE _pychron_sdk OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
    set(_pychron_sdk_libcxx "${_pychron_sdk}/usr/include/c++/v1")
    if(_pychron_sdk AND EXISTS "${_pychron_sdk_libcxx}/version")
      message(STATUS "pychron: toolchain libc++ headers incomplete; using ${_pychron_sdk_libcxx}")
      add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-nostdinc++>"
                          "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-isystem ${_pychron_sdk_libcxx}>")
    else()
      message(WARNING "pychron: <version> header not found and no SDK libc++ fallback found")
    endif()
  endif()
endif()
