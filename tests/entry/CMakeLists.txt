# One test_<unit>.cpp per entry unit. The tests that write go through a real
# store, on the engines tests/persistence/store_fixture.hpp provides.
if(NOT PYCHRON_PERSISTENCE_ENABLED)
  return()
endif()

file(GLOB PYCHRON_ENTRY_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

add_executable(pychron_entry_tests ${PYCHRON_ENTRY_TESTS})
# store_fixture.hpp opens a white-box connection, so it sees TinyORM and Qt.
target_include_directories(pychron_entry_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/tests/persistence"
  "${PROJECT_SOURCE_DIR}/libs/persistence/src")
# test_seed.cpp reads the seed an install ships.
target_compile_definitions(pychron_entry_tests PRIVATE QT_NO_KEYWORDS
  PYCHRON_PROFILES_DIR="${PROJECT_SOURCE_DIR}/profiles")
target_link_libraries(pychron_entry_tests PRIVATE pychron::entry pychron::persistence TinyOrm::TinyOrm Qt6::Core
  Qt6::Sql GTest::gtest_main)
pychron_set_warnings(pychron_entry_tests)
gtest_discover_tests(pychron_entry_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)
