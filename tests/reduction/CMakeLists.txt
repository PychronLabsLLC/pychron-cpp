# One test_<component>.cpp per reduction component. Globbed so parallel units only add files.
file(GLOB PYCHRON_REDUCTION_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

if(PYCHRON_REDUCTION_TESTS)
  add_executable(pychron_reduction_tests ${PYCHRON_REDUCTION_TESTS})
  target_link_libraries(pychron_reduction_tests PRIVATE pychron::reduction GTest::gtest_main)
  target_compile_definitions(pychron_reduction_tests PRIVATE
    PYCHRON_EXAMPLE_CONFIGS_DIR="${PROJECT_SOURCE_DIR}/configs/examples"
    PYCHRON_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data")
  pychron_set_warnings(pychron_reduction_tests)
  gtest_discover_tests(pychron_reduction_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
endif()
