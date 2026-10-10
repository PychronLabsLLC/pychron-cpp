# End-to-end: example configs with kind = "sim" -> ExtractionLine ->
# SimSystem -> actuate -> assert pressures; the measurement path over the sim
# line and spectrometer. Globbed so later suites only add files.
file(GLOB PYCHRON_INTEGRATION_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")
# The install end-to-end test needs the store and has its own executable (below).
list(FILTER PYCHRON_INTEGRATION_TESTS EXCLUDE REGEX "test_install_e2e\\.cpp$")

if(PYCHRON_INTEGRATION_TESTS)
  add_executable(pychron_integration_tests ${PYCHRON_INTEGRATION_TESTS})
  target_link_libraries(pychron_integration_tests PRIVATE pychron::experiment pychron::systems GTest::gtest_main)
  target_include_directories(pychron_integration_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
  target_compile_definitions(pychron_integration_tests PRIVATE
    PYCHRON_EXAMPLE_CONFIGS_DIR="${PROJECT_SOURCE_DIR}/configs/examples"
    PYCHRON_PLAN_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/tests/experiment/fixtures/plans/valid")
  if(TARGET pychron_metrics)
    # test_metrics_packaging.cpp reads packaging/observability; the metrics
    # tests scrape over a socket (http_client.hpp).
    target_compile_definitions(pychron_integration_tests PRIVATE PYCHRON_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(pychron_integration_tests PRIVATE asio::asio)
    if(WIN32)
      target_compile_definitions(pychron_integration_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    # The dashboards are checked as JSON where a JSON library is built (with persistence).
    if(TARGET nlohmann_json::nlohmann_json)
      target_link_libraries(pychron_integration_tests PRIVATE nlohmann_json::nlohmann_json)
      target_compile_definitions(pychron_integration_tests PRIVATE PYCHRON_TEST_HAS_JSON=1)
    endif()
  endif()
  # spectrometer sim drivers register from static initializers nothing references
  pychron_link_drivers(pychron_integration_tests pychron_sim)
  pychron_set_warnings(pychron_integration_tests)
  gtest_discover_tests(pychron_integration_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
endif()

# A fresh install's queue, run on simulated hardware into the install's
# database and read back as the data window reads it. Its database fixture
# (tests/persistence/store_fixture.hpp) makes a PostgreSQL schema through
# persistence's own connection, so sees TinyORM and Qt.
if(TARGET pychron_experiment_store AND TARGET pychron_processing_store AND TARGET pychron_processing_records
   AND TARGET pychron_entry)
  add_executable(pychron_install_e2e_tests test_install_e2e.cpp)
  target_include_directories(pychron_install_e2e_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support
    "${PROJECT_SOURCE_DIR}/tests/persistence" "${PROJECT_SOURCE_DIR}/libs/persistence/src")
  target_compile_definitions(pychron_install_e2e_tests PRIVATE QT_NO_KEYWORDS
    PYCHRON_EXAMPLE_CONFIGS_DIR="${PROJECT_SOURCE_DIR}/configs/examples"
    PYCHRON_PROFILES_DIR="${PROJECT_SOURCE_DIR}/profiles")
  target_link_libraries(pychron_install_e2e_tests PRIVATE pychron::experiment_store pychron::processing_store
    pychron::processing_records pychron::entry pychron::setup pychron::experiment pychron::systems TinyOrm::TinyOrm
    Qt6::Core Qt6::Sql nlohmann_json::nlohmann_json GTest::gtest_main)
  # the spectrometer sim drivers the install's simulation mode names
  pychron_link_drivers(pychron_install_e2e_tests pychron_sim)
  pychron_set_warnings(pychron_install_e2e_tests)
  gtest_discover_tests(pychron_install_e2e_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)
endif()
