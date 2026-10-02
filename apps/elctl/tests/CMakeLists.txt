file(GLOB PYCHRON_ELCTL_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

add_executable(pychron_elctl_tests ${PYCHRON_ELCTL_TESTS})
target_link_libraries(pychron_elctl_tests PRIVATE pychron_elctl_lib GTest::gtest_main)
pychron_link_drivers(pychron_elctl_tests pychron_sim)
target_compile_definitions(pychron_elctl_tests PRIVATE
  PYCHRON_EXAMPLE_CONFIGS_DIR="${PROJECT_SOURCE_DIR}/configs/examples")
pychron_set_warnings(pychron_elctl_tests)
gtest_discover_tests(pychron_elctl_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
