# One test_<component>.cpp per ingest component. The writer tests run against
# a real store, on the engines tests/persistence/store_fixture.hpp provides.
if(NOT PYCHRON_PERSISTENCE_ENABLED)
  return()
endif()

file(GLOB PYCHRON_INGEST_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

add_executable(pychron_ingest_tests ${PYCHRON_INGEST_TESTS})
# store_fixture.hpp opens a white-box connection, so it sees TinyORM and Qt.
target_include_directories(pychron_ingest_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/tests/persistence"
  "${PROJECT_SOURCE_DIR}/libs/persistence/src")
target_compile_definitions(pychron_ingest_tests PRIVATE QT_NO_KEYWORDS)
target_link_libraries(pychron_ingest_tests PRIVATE pychron::ingest pychron::persistence TinyOrm::TinyOrm Qt6::Core
  Qt6::Sql GTest::gtest_main)
pychron_set_warnings(pychron_ingest_tests)
gtest_discover_tests(pychron_ingest_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)
