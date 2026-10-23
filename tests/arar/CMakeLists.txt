# The Ar-Ar suites: reduction step functions, reduce(), decay, flux and the
# golden vectors under golden/ (spec 9.4); the figures, the report, group
# ages, the flux fit and view, and the domain. Globbed so parallel units only
# add files. The store suites (the flux store, the store source's Ar-Ar
# loader) need the persistence library and build only when the store adapter
# exists; the records loader suite, only when the records adapter does.
file(GLOB PYCHRON_ARAR_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")
list(FILTER PYCHRON_ARAR_TESTS EXCLUDE REGEX "test_(flux_store|store_loader|store_load_timing|record_loader)\\.cpp$")

if(PYCHRON_ARAR_TESTS)
  add_executable(pychron_arar_tests ${PYCHRON_ARAR_TESTS})
  target_link_libraries(pychron_arar_tests PRIVATE pychron::arar GTest::gtest_main)
  # tests/support: golden_json.hpp, the golden reader shared with
  # tests/reduction; tests/processing: fixtures.hpp, the analyses the
  # processing suites are built on (arar_fixtures.hpp adds their Ar-Ar data).
  target_include_directories(pychron_arar_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/tests/support
    ${PROJECT_SOURCE_DIR}/tests/processing)
  target_compile_definitions(pychron_arar_tests PRIVATE
    PYCHRON_EXAMPLE_CONFIGS_DIR="${PROJECT_SOURCE_DIR}/configs/examples"
    PYCHRON_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data"
    PYCHRON_REDUCTION_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")
  pychron_set_warnings(pychron_arar_tests)
  # -ffp-contract=off (unfused test arithmetic) comes PUBLIC from pychron::arar_reduction.
  gtest_discover_tests(pychron_arar_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
endif()

if(TARGET pychron_arar_store)
  add_executable(pychron_arar_store_tests test_flux_store.cpp test_store_loader.cpp test_store_load_timing.cpp)
  # The flux store tests run on every engine the persistence tests do; their
  # database fixture (tests/persistence/store_fixture.hpp) makes a PostgreSQL
  # schema through persistence's own connection, so sees TinyORM and Qt.
  # tests/processing: store_source_fixture.hpp, the store the store source
  # suites share. fixtures/: the input fingerprint recorded before the domain
  # seam moved the Ar-Ar inputs behind the loader.
  target_include_directories(pychron_arar_store_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    "${PROJECT_SOURCE_DIR}/tests/persistence" "${PROJECT_SOURCE_DIR}/libs/persistence/src"
    "${PROJECT_SOURCE_DIR}/tests/processing")
  target_compile_definitions(pychron_arar_store_tests PRIVATE QT_NO_KEYWORDS
    PYCHRON_ARAR_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
  target_link_libraries(pychron_arar_store_tests PRIVATE pychron::arar_store TinyOrm::TinyOrm Qt6::Core
    Qt6::Sql nlohmann_json::nlohmann_json GTest::gtest_main)
  pychron_set_warnings(pychron_arar_store_tests)
  gtest_discover_tests(pychron_arar_store_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)
endif()

if(TARGET pychron_arar_records)
  add_executable(pychron_arar_records_tests test_record_loader.cpp)
  # tests/processing: record_fixtures.hpp, the records the record source suites write.
  target_include_directories(pychron_arar_records_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/processing")
  target_link_libraries(pychron_arar_records_tests PRIVATE pychron::arar_records GTest::gtest_main)
  pychron_set_warnings(pychron_arar_records_tests)
  gtest_discover_tests(pychron_arar_records_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
endif()
