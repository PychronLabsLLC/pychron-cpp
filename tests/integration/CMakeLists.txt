# End-to-end: example configs with kind = "sim" -> ExtractionLine ->
# SimSystem -> actuate -> assert pressures; the measurement path over the sim
# line and spectrometer. Globbed so later suites only add files.
file(GLOB PYCHRON_INTEGRATION_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

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
