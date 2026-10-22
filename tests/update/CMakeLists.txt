# One test_<component>.cpp per update component. Globbed so parallel units
# only add files. Fixtures are GitHub answers kept in fixtures/.
file(GLOB PYCHRON_UPDATE_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

# FakeHttp, release_json() and the fixture directory, for tests that fake the
# update check's HTTP seam. The elctl and pychron-ui tests link this too.
add_library(pychron_update_testing INTERFACE)
add_library(pychron::update_testing ALIAS pychron_update_testing)
target_include_directories(pychron_update_testing INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(pychron_update_testing INTERFACE
  PYCHRON_UPDATE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
target_link_libraries(pychron_update_testing INTERFACE pychron::update GTest::gtest)

add_executable(pychron_update_tests ${PYCHRON_UPDATE_TESTS})
target_link_libraries(pychron_update_tests PRIVATE pychron::update_testing GTest::gtest_main)
pychron_set_warnings(pychron_update_tests)
gtest_discover_tests(pychron_update_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
