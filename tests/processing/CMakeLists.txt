# One test_<component>.cpp per processing component. The record-source suite
# and the store-source suite need the experiment and persistence libraries and
# build only when their adapters exist.
file(GLOB PYCHRON_PROCESSING_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")
list(FILTER PYCHRON_PROCESSING_TESTS EXCLUDE REGEX "test_(record_source|store_source|store_load_timing)\\.cpp$")

add_executable(pychron_processing_tests ${PYCHRON_PROCESSING_TESTS})
target_include_directories(pychron_processing_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/tests/support)
target_link_libraries(pychron_processing_tests PRIVATE pychron::processing GTest::gtest_main)
# The committed SVG and PDF of a scene (fixtures/scene): a stored format, so the
# bytes one compiler writes are checked under every other (root AGENTS.md).
target_compile_definitions(pychron_processing_tests PRIVATE
  PYCHRON_PROCESSING_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
pychron_set_warnings(pychron_processing_tests)
gtest_discover_tests(pychron_processing_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)

if(TARGET pychron_processing_records)
  add_executable(pychron_processing_records_tests test_record_source.cpp)
  # Temporary: pychron::arar for the age the record directory's references
  # give (arar_reduce); Task 10 moves that case to tests/arar and removes it.
  target_link_libraries(pychron_processing_records_tests PRIVATE pychron::processing_records pychron::arar
    GTest::gtest_main)
  pychron_set_warnings(pychron_processing_records_tests)
  gtest_discover_tests(pychron_processing_records_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
endif()

if(TARGET pychron_processing_store)
  add_executable(pychron_processing_store_tests test_store_source.cpp test_store_load_timing.cpp)
  # The store tests run on every engine the persistence tests do; their
  # database fixture (tests/persistence/store_fixture.hpp) makes a PostgreSQL
  # schema through persistence's own connection, so sees TinyORM and Qt.
  target_include_directories(pychron_processing_store_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    "${PROJECT_SOURCE_DIR}/tests/persistence" "${PROJECT_SOURCE_DIR}/libs/persistence/src")
  target_compile_definitions(pychron_processing_store_tests PRIVATE QT_NO_KEYWORDS)
  # Temporary: pychron::arar for the Ar-Ar cases of the store source suite
  # (the Ar-Ar domain, derived_age); Task 10 moves them to tests/arar and
  # removes it.
  target_link_libraries(pychron_processing_store_tests PRIVATE pychron::processing_store pychron::arar
    TinyOrm::TinyOrm Qt6::Core Qt6::Sql nlohmann_json::nlohmann_json GTest::gtest_main)
  pychron_set_warnings(pychron_processing_store_tests)
  gtest_discover_tests(pychron_processing_store_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)
endif()
