# One test_<component>.cpp per experiment component. Globbed so parallel units only add files.
file(GLOB PYCHRON_EXPERIMENT_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

if(PYCHRON_EXPERIMENT_TESTS)
  add_executable(pychron_experiment_tests ${PYCHRON_EXPERIMENT_TESTS})
  target_link_libraries(pychron_experiment_tests PRIVATE pychron::experiment GTest::gtest_main)
  target_compile_definitions(pychron_experiment_tests PRIVATE
    PYCHRON_EXAMPLE_CONFIGS_DIR="${PROJECT_SOURCE_DIR}/configs/examples"
    PYCHRON_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data")
  pychron_set_warnings(pychron_experiment_tests)
  gtest_discover_tests(pychron_experiment_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
endif()
