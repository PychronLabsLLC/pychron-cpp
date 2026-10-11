# One test_<component>.cpp per experiment component. Globbed so parallel units only add files.
file(GLOB PYCHRON_EXPERIMENT_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

# The record-ingest suite needs the store adapter and builds only with it.
list(FILTER PYCHRON_EXPERIMENT_TESTS EXCLUDE REGEX "test_(record_ingest|store_persister)\\.cpp$")

if(NOT TARGET pychron_metrics)
  list(FILTER PYCHRON_EXPERIMENT_TESTS EXCLUDE REGEX "test_(experiment_metrics|metrics_service)\\.cpp$")
endif()

if(PYCHRON_EXPERIMENT_TESTS)
  add_executable(pychron_experiment_tests ${PYCHRON_EXPERIMENT_TESTS})
  target_link_libraries(pychron_experiment_tests PRIVATE pychron::experiment GTest::gtest_main)
  target_compile_definitions(pychron_experiment_tests PRIVATE
    PYCHRON_EXAMPLE_CONFIGS_DIR="${PROJECT_SOURCE_DIR}/configs/examples"
    PYCHRON_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data"
    PYCHRON_PLAN_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/plans")
  # virtual_time.hpp
  target_include_directories(pychron_experiment_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
  if(TARGET pychron_metrics)
    # test_metrics_service.cpp scrapes over a socket (http_client.hpp)
    target_link_libraries(pychron_experiment_tests PRIVATE asio::asio)
    if(WIN32)
      target_compile_definitions(pychron_experiment_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
  endif()
  pychron_set_warnings(pychron_experiment_tests)
  gtest_discover_tests(pychron_experiment_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
endif()

if(TARGET pychron_experiment_store)
  add_executable(pychron_experiment_store_tests test_record_ingest.cpp test_store_persister.cpp)
  # The persister suite runs on every engine the persistence tests do; their
  # database fixture (tests/persistence/store_fixture.hpp) makes a PostgreSQL
  # schema through persistence's own connection, so sees TinyORM and Qt.
  target_include_directories(pychron_experiment_store_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    "${PROJECT_SOURCE_DIR}/tests/persistence" "${PROJECT_SOURCE_DIR}/libs/persistence/src")
  target_compile_definitions(pychron_experiment_store_tests PRIVATE QT_NO_KEYWORDS)
  target_link_libraries(pychron_experiment_store_tests PRIVATE pychron::experiment_store TinyOrm::TinyOrm Qt6::Core
    Qt6::Sql nlohmann_json::nlohmann_json GTest::gtest_main)
  pychron_set_warnings(pychron_experiment_store_tests)
  gtest_discover_tests(pychron_experiment_store_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)
endif()
