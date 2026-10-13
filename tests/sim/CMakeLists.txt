# One test_<component>.cpp per sim component. Globbed so parallel units only add files.
file(GLOB PYCHRON_SIM_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

if(PYCHRON_SIM_TESTS)
  add_executable(pychron_sim_tests ${PYCHRON_SIM_TESTS})
  target_link_libraries(pychron_sim_tests PRIVATE pychron::sim GTest::gtest_main)
  # virtual_time.hpp
  target_include_directories(pychron_sim_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
  target_compile_definitions(pychron_sim_tests PRIVATE
    PYCHRON_EXAMPLE_CONFIGS_DIR="${PROJECT_SOURCE_DIR}/configs/examples"
    PYCHRON_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data")
  # sim drivers register from static initializers nothing references by symbol
  pychron_link_drivers(pychron_sim_tests pychron_sim)
  pychron_set_warnings(pychron_sim_tests)
  gtest_discover_tests(pychron_sim_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
endif()
