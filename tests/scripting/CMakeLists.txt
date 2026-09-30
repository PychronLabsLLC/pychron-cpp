# One test_<component>.cpp per scripting component. Globbed so parallel units only add files.
file(GLOB PYCHRON_SCRIPTING_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

if(PYCHRON_SCRIPTING_TESTS)
  add_executable(pychron_scripting_tests ${PYCHRON_SCRIPTING_TESTS})
  target_link_libraries(pychron_scripting_tests PRIVATE pychron::scripting GTest::gtest_main)
  target_compile_definitions(pychron_scripting_tests PRIVATE
    PYCHRON_EXAMPLE_CONFIGS_DIR="${PROJECT_SOURCE_DIR}/configs/examples"
    PYCHRON_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data")
  pychron_set_warnings(pychron_scripting_tests)
  gtest_discover_tests(pychron_scripting_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
endif()
