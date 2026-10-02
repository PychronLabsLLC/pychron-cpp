# One test_<component>.cpp per persistence component. PostgreSQL cases run only
# when PYCHRON_TEST_PG_URL is set (e.g. postgresql://user:pw@localhost/pychron_test);
# SQLite cases always run.
if(NOT PYCHRON_PERSISTENCE_ENABLED)
  return()
endif()

file(GLOB PYCHRON_PERSISTENCE_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

add_executable(pychron_persistence_tests ${PYCHRON_PERSISTENCE_TESTS})
# White-box tests (DDL parity, triggers, statement splitting) reach into src/
# and therefore see TinyORM and Qt.
target_include_directories(pychron_persistence_tests PRIVATE "${PROJECT_SOURCE_DIR}/libs/persistence/src")
target_compile_definitions(pychron_persistence_tests PRIVATE QT_NO_KEYWORDS)
target_link_libraries(pychron_persistence_tests PRIVATE pychron::persistence TinyOrm::TinyOrm Qt6::Core Qt6::Sql
  GTest::gtest_main)
pychron_set_warnings(pychron_persistence_tests)
gtest_discover_tests(pychron_persistence_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)
