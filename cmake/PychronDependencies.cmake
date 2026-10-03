# Third-party dependencies. find_package() is tried first (vcpkg manifest mode
# via the presets); if a package is not installed, FetchContent builds it from
# a pinned release so a plain `cmake -S . -B build` also works.
include(FetchContent)

FetchContent_Declare(tomlplusplus
  URL https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.4.0.tar.gz
  URL_HASH SHA256=8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/patches/tomlplusplus_float_columns.cmake
  FIND_PACKAGE_ARGS CONFIG)
FetchContent_MakeAvailable(tomlplusplus)

# Logging back end (spec 4.6). Bundled fmt keeps fmt out of every public
# interface; pychron_core links spdlog PRIVATE.
FetchContent_Declare(spdlog
  URL https://github.com/gabime/spdlog/archive/refs/tags/v1.15.3.tar.gz
  URL_HASH SHA256=15a04e69c222eb6c01094b5c7ff8a249b36bb22788d72519646fb85feb267e67
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  FIND_PACKAGE_ARGS CONFIG)
set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
# Third-party headers must not trip PYCHRON_WARNINGS_AS_ERRORS.
set(SPDLOG_SYSTEM_INCLUDES ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(spdlog)

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

# Optional OpenCV for libs/vision (LegacyFinder, OpenCvSource). AUTO uses it
# when found, ON requires it, OFF never looks. There is no fetch fallback: a
# source build is too heavy for CI. Without it the two OpenCV translation
# units compile to stubs, so the source list never changes.
set(PYCHRON_VISION_OPENCV "AUTO" CACHE STRING "OpenCV for libs/vision: AUTO, ON or OFF")
set_property(CACHE PYCHRON_VISION_OPENCV PROPERTY STRINGS AUTO ON OFF)
set(PYCHRON_VISION_OPENCV_ENABLED OFF)
if(PYCHRON_VISION AND NOT PYCHRON_VISION_OPENCV STREQUAL "OFF")
  if(NOT PYCHRON_VISION_OPENCV STREQUAL "AUTO" AND NOT PYCHRON_VISION_OPENCV STREQUAL "ON")
    message(FATAL_ERROR "PYCHRON_VISION_OPENCV must be AUTO, ON or OFF (got '${PYCHRON_VISION_OPENCV}')")
  endif()
  # The package is called OpenCV in 4.x and 5.x. In 5.x the contour and
  # geometry functions moved into the geometry module.
  find_package(OpenCV QUIET COMPONENTS core imgproc videoio)
  if(OpenCV_FOUND AND OpenCV_VERSION VERSION_GREATER_EQUAL 5)
    find_package(OpenCV QUIET COMPONENTS core imgproc videoio geometry)
  endif()
  if(OpenCV_FOUND)
    set(PYCHRON_VISION_OPENCV_ENABLED ON)
  elseif(PYCHRON_VISION_OPENCV STREQUAL "ON")
    message(FATAL_ERROR "PYCHRON_VISION_OPENCV=ON but OpenCV (core, imgproc, videoio) was not found")
  endif()
  if(PYCHRON_VISION_OPENCV_ENABLED)
    message(STATUS "PYCHRON_VISION_OPENCV=${PYCHRON_VISION_OPENCV}: using OpenCV ${OpenCV_VERSION}")
  else()
    message(STATUS "PYCHRON_VISION_OPENCV=${PYCHRON_VISION_OPENCV}: OpenCV not found; building stubs")
  endif()
elseif(PYCHRON_VISION)
  message(STATUS "PYCHRON_VISION_OPENCV=OFF: building stubs")
endif()

# QCustomPlot (GPL) for the UI strip charts. Ships no CMake project, so the
# static library is defined here. UI only: Qt must already be found.
if(BUILD_UI)
  find_package(Qt6 REQUIRED COMPONENTS Widgets PrintSupport)
  FetchContent_Declare(qcustomplot
    URL https://www.qcustomplot.com/release/2.1.1/QCustomPlot-source.tar.gz
    URL_HASH SHA256=5e2d22dec779db8f01f357cbdb25e54fbcf971adaee75eae8d7ad2444487182f
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(qcustomplot)

  add_library(qcustomplot STATIC
    "${qcustomplot_SOURCE_DIR}/qcustomplot.cpp"
    "${qcustomplot_SOURCE_DIR}/qcustomplot.h")
  set_target_properties(qcustomplot PROPERTIES AUTOMOC ON)
  target_include_directories(qcustomplot SYSTEM PUBLIC "${qcustomplot_SOURCE_DIR}")
  target_link_libraries(qcustomplot PUBLIC Qt6::Widgets Qt6::PrintSupport)
  # Third-party code: not ours to keep warning-clean.
  target_compile_options(qcustomplot PRIVATE -w)
endif()

# DVC persistence (libs/persistence) on TinyORM over QtSql (QSQLITE, QPSQL).
# Qt stays behind the library boundary: TinyORM and Qt are PRIVATE
# dependencies and no public persistence header includes them. The library is
# built only when Qt6 Core and Sql are found; elsewhere it is skipped.
option(PYCHRON_PERSISTENCE "Build libs/persistence (needs Qt6 Core + Sql; TinyORM is fetched)" ON)
set(PYCHRON_PERSISTENCE_ENABLED OFF)
if(PYCHRON_PERSISTENCE)
  find_package(Qt6 6.2 QUIET COMPONENTS Core Sql)
  if(Qt6Sql_FOUND)
    # TinyORM's own CMake calls find_package(range-v3 CONFIG REQUIRED);
    # OVERRIDE_FIND_PACKAGE lets the fetched copy satisfy it. Git sources are
    # pinned to the release commit.
    FetchContent_Declare(range-v3
      GIT_REPOSITORY https://github.com/ericniebler/range-v3.git
      GIT_TAG 8c88f7174bcc71e525015430282cd7b984f8be47  # 0.12.0
      SYSTEM
      OVERRIDE_FIND_PACKAGE)
    set(RANGE_V3_TESTS OFF CACHE BOOL "" FORCE)
    set(RANGE_V3_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(RANGE_V3_PERF OFF CACHE BOOL "" FORCE)
    set(RANGE_V3_DOCS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(range-v3)

    # Query builder + ORM only: no tom CLI (would pull in tabulate), no
    # TinyDrivers (QtSql drivers are used), static library.
    set(TOM OFF CACHE BOOL "" FORCE)
    set(TOM_EXAMPLE OFF CACHE BOOL "" FORCE)
    set(BUILD_DRIVERS OFF CACHE BOOL "" FORCE)
    # TinyORM's BUILD_TESTS option shares our cache variable's name: shadow it
    # with a normal variable (CMP0077) so its own test suite is not built.
    set(_pychron_shared ${BUILD_SHARED_LIBS})
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_TESTS OFF)
    FetchContent_Declare(TinyOrm
      GIT_REPOSITORY https://github.com/silverqx/TinyORM.git
      GIT_TAG d568759812199c095f5a7c96d5111264a8f1ac83  # v0.38.1
      SYSTEM
      FIND_PACKAGE_ARGS CONFIG)
    FetchContent_MakeAvailable(TinyOrm)
    set(BUILD_SHARED_LIBS ${_pychron_shared})
    unset(BUILD_TESTS)

    # Third-party build policy is not ours: TinyORM adds -Werror (Debug) and
    # logs every query to qDebug in Debug builds. Drop both.
    if(TARGET CommonConfig)
      get_target_property(_tiny_opts CommonConfig INTERFACE_COMPILE_OPTIONS)
      if(_tiny_opts)
        list(TRANSFORM _tiny_opts REPLACE "-Werror|-Wfatal-errors|-pedantic-errors|/WX" "")
        set_target_properties(CommonConfig PROPERTIES INTERFACE_COMPILE_OPTIONS "${_tiny_opts}")
      endif()
      get_target_property(_tiny_link CommonConfig INTERFACE_LINK_OPTIONS)
      if(_tiny_link)
        list(TRANSFORM _tiny_link REPLACE "/WX" "")
        set_target_properties(CommonConfig PROPERTIES INTERFACE_LINK_OPTIONS "${_tiny_link}")
      endif()
    endif()
    if(TARGET TinyOrm)
      get_target_property(_tiny_defs TinyOrm INTERFACE_COMPILE_DEFINITIONS)
      list(TRANSFORM _tiny_defs REPLACE "TINYORM_DEBUG_SQL" "TINYORM_NO_DEBUG_SQL")
      set_target_properties(TinyOrm PROPERTIES INTERFACE_COMPILE_DEFINITIONS "${_tiny_defs}")
      get_target_property(_tiny_defs TinyOrm COMPILE_DEFINITIONS)
      list(TRANSFORM _tiny_defs REPLACE "TINYORM_DEBUG_SQL" "TINYORM_NO_DEBUG_SQL")
      set_target_properties(TinyOrm PROPERTIES COMPILE_DEFINITIONS "${_tiny_defs}")
    endif()
    set(PYCHRON_PERSISTENCE_ENABLED ON)
  else()
    message(WARNING "PYCHRON_PERSISTENCE: Qt6 Core/Sql not found; libs/persistence is not built")
  endif()
endif()
