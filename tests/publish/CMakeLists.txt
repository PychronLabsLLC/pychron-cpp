# One test_<component>.cpp per publish component, plus the elctl publish and
# embargo commands (the first tests to drive pychron_elctl_lib). The release
# fixture is a store (tests/persistence/store_fixture.hpp, which needs Qt and
# TinyORM): SQLite always, PostgreSQL as well when PYCHRON_TEST_PG_URL is set.
if(NOT PYCHRON_PERSISTENCE_ENABLED)
  return()
endif()

file(GLOB PYCHRON_PUBLISH_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

add_executable(pychron_publish_tests ${PYCHRON_PUBLISH_TESTS})
target_include_directories(pychron_publish_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..
  "${PROJECT_SOURCE_DIR}/libs/persistence/src")
target_compile_definitions(pychron_publish_tests PRIVATE QT_NO_KEYWORDS
  PYCHRON_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
  PYCHRON_PUBLISH_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
target_link_libraries(pychron_publish_tests PRIVATE pychron::publish pychron::processing pychron::reduction
  pychron_elctl_lib TinyOrm::TinyOrm Qt6::Core Qt6::Sql GTest::gtest_main)
pychron_set_warnings(pychron_publish_tests)
gtest_discover_tests(pychron_publish_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 300)
