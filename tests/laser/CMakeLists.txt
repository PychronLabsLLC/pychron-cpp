# One test_<component>.cpp per laser-system component.
file(GLOB PYCHRON_LASER_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

add_executable(pychron_laser_tests ${PYCHRON_LASER_TESTS})
target_link_libraries(pychron_laser_tests PRIVATE pychron::laser GTest::gtest_main)
target_include_directories(pychron_laser_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/devices")
target_compile_definitions(pychron_laser_tests PRIVATE
  PYCHRON_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data")
pychron_set_warnings(pychron_laser_tests)

gtest_discover_tests(pychron_laser_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
