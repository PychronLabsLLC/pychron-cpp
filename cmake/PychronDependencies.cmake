# Third-party dependencies. find_package() is tried first (vcpkg manifest mode
# via the presets); if a package is not installed, FetchContent builds it from
# a pinned release so a plain `cmake -S . -B build` also works.
include(FetchContent)

FetchContent_Declare(tomlplusplus
  URL https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.4.0.tar.gz
  URL_HASH SHA256=8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  FIND_PACKAGE_ARGS CONFIG)
FetchContent_MakeAvailable(tomlplusplus)

if(BUILD_TESTS)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    FIND_PACKAGE_ARGS NAMES GTest)
  FetchContent_MakeAvailable(googletest)
endif()

# Embedded CPython for libs/scripting (spec section 6). With the option off,
# or when no Python >= 3.12 with embedding support is found, libs/scripting
# builds its stub host that rejects scripted runs.
option(PYCHRON_SCRIPTING "Embed CPython (pybind11) in libs/scripting" ON)
set(PYCHRON_SCRIPTING_ENABLED OFF)
if(PYCHRON_SCRIPTING)
  find_package(Python 3.12 COMPONENTS Interpreter Development.Embed)
  if(Python_FOUND)
    set(PYBIND11_FINDPYTHON ON CACHE BOOL "" FORCE)
    FetchContent_Declare(pybind11
      URL https://github.com/pybind/pybind11/archive/refs/tags/v3.0.1.tar.gz
      URL_HASH SHA256=741633da746b7c738bb71f1854f957b9da660bcd2dce68d71949037f0969d0ca
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
      SYSTEM
      FIND_PACKAGE_ARGS 3 CONFIG)
    FetchContent_MakeAvailable(pybind11)
    set(PYCHRON_SCRIPTING_ENABLED ON)
  else()
    message(WARNING "PYCHRON_SCRIPTING: Python >= 3.12 (Development.Embed) not found; building the stub script host")
  endif()
endif()
