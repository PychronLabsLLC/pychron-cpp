cmake_minimum_required(VERSION 3.25)

project(pychron_cpp VERSION 0.3.0 LANGUAGES CXX)  # x-release-please-version

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BUILD_UI "Build the Qt6 UI (apps/pychron-ui)" OFF)
option(PYCHRON_VISION "Build libs/vision" ON)
option(PYCHRON_METRICS "Build libs/metrics (the Prometheus endpoint)" ON)
option(BUILD_TESTS "Build GoogleTest suites under tests/" ON)
option(PYCHRON_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
set(PYCHRON_SANITIZE "" CACHE STRING "Comma-separated sanitizers, e.g. address,undefined")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(PychronToolchainFixes)
include(PychronWarnings)
include(PychronSanitizers)
include(PychronDependencies)

# Libraries in dependency order: core <- transport <- codecs <- devices <- systems.
# persistence depends on core only (DVC schema spec, section 12.1).
# sim depends on transport and devices only. A library is added once its
# directory exists so each unit can land independently.
set(PYCHRON_LIBS core metrics vision persistence ingest dvc entry transport codecs devices laser sim systems reduction scripting experiment processing setup)

if(NOT PYCHRON_METRICS)
  list(REMOVE_ITEM PYCHRON_LIBS metrics)
endif()

foreach(lib IN LISTS PYCHRON_LIBS)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/${lib}/CMakeLists.txt")
    add_subdirectory(libs/${lib})
  endif()
endforeach()

foreach(app IN ITEMS elctl)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/apps/${app}/CMakeLists.txt")
    add_subdirectory(apps/${app})
  endif()
endforeach()

if(BUILD_UI AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/apps/pychron-ui/CMakeLists.txt")
  add_subdirectory(apps/pychron-ui)
endif()

if(BUILD_TESTS)
  enable_testing()
  include(GoogleTest)
  foreach(suite IN LISTS PYCHRON_LIBS ITEMS integration)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/${suite}/CMakeLists.txt")
      add_subdirectory(tests/${suite})
    endif()
  endforeach()
  if(BUILD_UI AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/ui/CMakeLists.txt")
    add_subdirectory(tests/ui)
  endif()
endif()

# Install rules and packages (cpack); see cmake/PychronInstall.cmake.
include(PychronInstall)
