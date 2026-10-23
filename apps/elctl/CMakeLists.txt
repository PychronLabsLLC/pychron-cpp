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
# al. 2021 report, read from the store), `elctl flux` and `elctl db` (the
# store's schema) are split the same way.
# So is `elctl earthbank` (the EarthBank workbooks, filled from the store).
# src/selection.cpp (the analyses `export` and `earthbank` select, read from
# the store) has no stub: nothing uses it without persistence.
set(PYCHRON_ELCTL_IMPORT_STUB "${CMAKE_CURRENT_SOURCE_DIR}/src/import_stub.cpp")
set(PYCHRON_ELCTL_ENTRY_STUB "${CMAKE_CURRENT_SOURCE_DIR}/src/entry_stub.cpp")
set(PYCHRON_ELCTL_EXPORT_STUB "${CMAKE_CURRENT_SOURCE_DIR}/src/export_stub.cpp")
set(PYCHRON_ELCTL_EARTHBANK_STUB "${CMAKE_CURRENT_SOURCE_DIR}/src/earthbank_stub.cpp")
set(PYCHRON_ELCTL_FLUX_STUB "${CMAKE_CURRENT_SOURCE_DIR}/src/flux_stub.cpp")
set(PYCHRON_ELCTL_DB_STUB "${CMAKE_CURRENT_SOURCE_DIR}/src/db_stub.cpp")
# `elctl publish` and `elctl embargo` (data publishing spec 6.2) are split the same way.
set(PYCHRON_ELCTL_PUBLISH_STUB "${CMAKE_CURRENT_SOURCE_DIR}/src/publish_stub.cpp")
set(PYCHRON_ELCTL_EMBARGO_STUB "${CMAKE_CURRENT_SOURCE_DIR}/src/embargo_stub.cpp")
# `exp run`'s database (src/exp_store.cpp) is split the same way.
set(PYCHRON_ELCTL_EXP_STORE_STUB "${CMAKE_CURRENT_SOURCE_DIR}/src/exp_store_stub.cpp")
# `elctl earthbank login|account|packages|preflight` talk to the service through
# libs/earthbank_qt (Qt6 Network). Without it src/earthbank_no_network.cpp
# gives make_http_client() as null and those commands say so and exit 2.
set(PYCHRON_ELCTL_NO_NETWORK "${CMAKE_CURRENT_SOURCE_DIR}/src/earthbank_no_network.cpp")
list(REMOVE_ITEM PYCHRON_ELCTL_SOURCES "${PYCHRON_ELCTL_NO_NETWORK}")
if(TARGET pychron_persistence)
  list(REMOVE_ITEM PYCHRON_ELCTL_SOURCES "${PYCHRON_ELCTL_IMPORT_STUB}" "${PYCHRON_ELCTL_ENTRY_STUB}"
    "${PYCHRON_ELCTL_EXPORT_STUB}" "${PYCHRON_ELCTL_EARTHBANK_STUB}" "${PYCHRON_ELCTL_FLUX_STUB}"
    "${PYCHRON_ELCTL_DB_STUB}" "${PYCHRON_ELCTL_EXP_STORE_STUB}" "${PYCHRON_ELCTL_PUBLISH_STUB}"
    "${PYCHRON_ELCTL_EMBARGO_STUB}")
  if(NOT TARGET pychron_earthbank_qt)
    list(APPEND PYCHRON_ELCTL_SOURCES "${PYCHRON_ELCTL_NO_NETWORK}")
  endif()
else()
  list(FILTER PYCHRON_ELCTL_SOURCES EXCLUDE REGEX "/src/(import|entry|export|earthbank|flux|db|exp_store|selection|publish|embargo)[^/]*\\.cpp$")
  list(APPEND PYCHRON_ELCTL_SOURCES "${PYCHRON_ELCTL_IMPORT_STUB}" "${PYCHRON_ELCTL_ENTRY_STUB}"
    "${PYCHRON_ELCTL_EXPORT_STUB}" "${PYCHRON_ELCTL_EARTHBANK_STUB}" "${PYCHRON_ELCTL_FLUX_STUB}"
    "${PYCHRON_ELCTL_DB_STUB}" "${PYCHRON_ELCTL_EXP_STORE_STUB}" "${PYCHRON_ELCTL_PUBLISH_STUB}"
    "${PYCHRON_ELCTL_EMBARGO_STUB}")
endif()

add_library(pychron_elctl_lib STATIC ${PYCHRON_ELCTL_SOURCES})
target_include_directories(pychron_elctl_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(pychron_elctl_lib PUBLIC cxx_std_20)
# processing and arar: the domains every command runs on (src/domain.cpp), in
# every build.
target_link_libraries(pychron_elctl_lib PUBLIC pychron::setup pychron::experiment pychron::systems pychron::devices
  pychron::processing pychron::arar pychron::transport pychron::update pychron::core)
if(TARGET pychron_persistence)
  # A data reduction install's local database is created with the DVC store.
  # processing_store: the reduction `import verify` compares legacy ages with,
  # and the source `export` and `earthbank` read. arar_store: the flux store
  # `flux` reads and writes. earthbank: the submissions and the workbooks; it
  # has no network code.
  target_link_libraries(pychron_elctl_lib PUBLIC pychron_persistence pychron::dvc pychron::ingest pychron::entry
    pychron::processing_store pychron::arar_store pychron::experiment_store pychron::earthbank pychron::publish
    PRIVATE nlohmann_json::nlohmann_json)
  target_compile_definitions(pychron_elctl_lib PUBLIC PYCHRON_ELCTL_HAS_STORE=1 PYCHRON_ELCTL_IMPORT
    PRIVATE PYCHRON_ELCTL_VERSION="${PROJECT_VERSION}")
endif()
if(TARGET pychron_earthbank_qt)
  target_link_libraries(pychron_elctl_lib PUBLIC pychron::earthbank_qt)
  target_compile_definitions(pychron_elctl_lib PUBLIC PYCHRON_ELCTL_HAS_NETWORK=1)
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
