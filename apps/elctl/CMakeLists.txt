# elctl: headless CLI for bring-up and scripting (spec section 9.3).
#
# Everything but main() lives in pychron_elctl_lib so the tests under
# tests/ drive the exact command code the binary runs.
file(GLOB PYCHRON_ELCTL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(REMOVE_ITEM PYCHRON_ELCTL_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

# `elctl import` (legacy ingestion spec, section 5) needs the DVC store. With
# persistence its sources are src/import*.cpp; without, only import_stub.cpp,
# which says so and exits 2. Never both.
# `elctl entry` (entry spec, section 10), `elctl export` (the Schaen et
# al. 2021 report, read from the store) and `elctl flux` are split the same way.
set(PYCHRON_ELCTL_IMPORT_STUB "${CMAKE_CURRENT_SOURCE_DIR}/src/import_stub.cpp")
set(PYCHRON_ELCTL_ENTRY_STUB "${CMAKE_CURRENT_SOURCE_DIR}/src/entry_stub.cpp")
set(PYCHRON_ELCTL_EXPORT_STUB "${CMAKE_CURRENT_SOURCE_DIR}/src/export_stub.cpp")
set(PYCHRON_ELCTL_FLUX_STUB "${CMAKE_CURRENT_SOURCE_DIR}/src/flux_stub.cpp")
if(TARGET pychron_persistence)
  list(REMOVE_ITEM PYCHRON_ELCTL_SOURCES "${PYCHRON_ELCTL_IMPORT_STUB}" "${PYCHRON_ELCTL_ENTRY_STUB}"
    "${PYCHRON_ELCTL_EXPORT_STUB}" "${PYCHRON_ELCTL_FLUX_STUB}")
else()
  list(FILTER PYCHRON_ELCTL_SOURCES EXCLUDE REGEX "/src/(import|entry|export|flux)[^/]*\\.cpp$")
  list(APPEND PYCHRON_ELCTL_SOURCES "${PYCHRON_ELCTL_IMPORT_STUB}" "${PYCHRON_ELCTL_ENTRY_STUB}"
    "${PYCHRON_ELCTL_EXPORT_STUB}" "${PYCHRON_ELCTL_FLUX_STUB}")
endif()

add_library(pychron_elctl_lib STATIC ${PYCHRON_ELCTL_SOURCES})
target_include_directories(pychron_elctl_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(pychron_elctl_lib PUBLIC cxx_std_20)
target_link_libraries(pychron_elctl_lib PUBLIC pychron::setup pychron::experiment pychron::systems pychron::devices
  pychron::transport pychron::core)
if(TARGET pychron_persistence)
  # A data reduction install's local database is created with the DVC store.
  # processing_store: the reduction `import verify` compares legacy ages with,
  # and the source `export` reads.
  target_link_libraries(pychron_elctl_lib PUBLIC pychron_persistence pychron::dvc pychron::ingest pychron::entry
    pychron::processing_store pychron::processing
    PRIVATE nlohmann_json::nlohmann_json)
  target_compile_definitions(pychron_elctl_lib PUBLIC PYCHRON_ELCTL_HAS_STORE=1 PYCHRON_ELCTL_IMPORT
    PRIVATE PYCHRON_ELCTL_VERSION="${PROJECT_VERSION}")
endif()
pychron_set_warnings(pychron_elctl_lib)

add_executable(elctl src/main.cpp)
target_link_libraries(elctl PRIVATE pychron_elctl_lib)
# sim spectrometer drivers register from static initializers nothing references
pychron_link_drivers(elctl pychron_sim)
pychron_set_warnings(elctl)

# App tests live beside the app: the top-level tests/ loop only covers libs/.
if(BUILD_TESTS)
  enable_testing()
  include(GoogleTest)
  add_subdirectory(tests)
endif()
