# End-to-end: example configs with kind = "sim" -> ExtractionLine ->
# SimSystem -> actuate -> assert pressures. Globbed so later suites only add files.
file(GLOB PYCHRON_INTEGRATION_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

if(PYCHRON_INTEGRATION_TESTS)
  add_executable(pychron_integration_tests ${PYCHRON_INTEGRATION_TESTS})
  target_link_libraries(pychron_integration_tests PRIVATE pychron::systems GTest::gtest_main)
  target_compile_definitions(pychron_integration_tests PRIVATE
    PYCHRON_EXAMPLE_CONFIGS_DIR="${PROJECT_SOURCE_DIR}/configs/examples")
  pychron_set_warnings(pychron_integration_tests)
  gtest_discover_tests(pychron_integration_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
endif()
