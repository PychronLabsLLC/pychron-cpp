# One test_<component>.cpp per processing component. The record-source suite
# and the store-source suite need the experiment and persistence libraries and
# build only when their adapters exist.
file(GLOB PYCHRON_PROCESSING_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")
list(FILTER PYCHRON_PROCESSING_TESTS EXCLUDE REGEX "test_(record_source|store_source|flux_store|store_load_timing)\\.cpp$")

add_executable(pychron_processing_tests ${PYCHRON_PROCESSING_TESTS})
target_include_directories(pychron_processing_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pychron_processing_tests PRIVATE pychron::processing GTest::gtest_main)
pychron_set_warnings(pychron_processing_tests)
gtest_discover_tests(pychron_processing_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)

if(TARGET pychron_processing_records)
  add_executable(pychron_processing_records_tests test_record_source.cpp)
  target_link_libraries(pychron_processing_records_tests PRIVATE pychron::processing_records GTest::gtest_main)
  pychron_set_warnings(pychron_processing_records_tests)
  gtest_discover_tests(pychron_processing_records_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
endif()

if(TARGET pychron_processing_store)
  add_executable(pychron_processing_store_tests test_store_source.cpp test_flux_store.cpp test_store_load_timing.cpp)
  # The flux store tests run on every engine the persistence tests do; their
  # database fixture (tests/persistence/store_fixture.hpp) makes a PostgreSQL
  # schema through persistence's own connection, so sees TinyORM and Qt.
  target_include_directories(pychron_processing_store_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    "${PROJECT_SOURCE_DIR}/tests/persistence" "${PROJECT_SOURCE_DIR}/libs/persistence/src")
  target_compile_definitions(pychron_processing_store_tests PRIVATE QT_NO_KEYWORDS)
  target_link_libraries(pychron_processing_store_tests PRIVATE pychron::processing_store TinyOrm::TinyOrm Qt6::Core
    Qt6::Sql nlohmann_json::nlohmann_json GTest::gtest_main)
  pychron_set_warnings(pychron_processing_store_tests)
  gtest_discover_tests(pychron_processing_store_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)
endif()
