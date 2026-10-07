# One test_<component>.cpp per laser-system component.
file(GLOB PYCHRON_LASER_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

add_executable(pychron_laser_tests ${PYCHRON_LASER_TESTS})
target_link_libraries(pychron_laser_tests PRIVATE pychron::laser GTest::gtest_main)
target_include_directories(pychron_laser_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/devices"
  "${PROJECT_SOURCE_DIR}/tests/support")
target_compile_definitions(pychron_laser_tests PRIVATE
  PYCHRON_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data")
pychron_set_warnings(pychron_laser_tests)

gtest_discover_tests(pychron_laser_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)

# tools/export_patterns.py (legacy .lp pickles to pattern TOML) is tested with
# the standard library's unittest; with elctl built, what it writes is also
# read back by the real loader. Python 3.11 has tomllib.
find_package(Python3 3.11 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME tools.export_patterns
    COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s "${PROJECT_SOURCE_DIR}/tools/tests" -p "test_export_patterns.py"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(tools.export_patterns PROPERTIES TIMEOUT 120)
  if(TARGET elctl)
    set_tests_properties(tools.export_patterns PROPERTIES ENVIRONMENT "ELCTL=$<TARGET_FILE:elctl>")
  endif()
else()
  message(STATUS "python3 >= 3.11 not found: tools/export_patterns.py is not tested")
endif()
