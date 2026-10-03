# One test_<component>.cpp per dvc component. The layout tests read the real
# legacy files under fixtures/ (see fixtures/README.md) and need no store; the
# import tests run on the engines tests/persistence/store_fixture.hpp provides.
if(NOT PYCHRON_PERSISTENCE_ENABLED)
  return()
endif()

file(GLOB PYCHRON_DVC_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

add_executable(pychron_dvc_tests ${PYCHRON_DVC_TESTS})
# test_legacy_json.cpp tests the private reader, so it sees src/ and the JSON
# library. test_project_import.cpp writes to a real store through
# tests/persistence/store_fixture.hpp, which opens a white-box connection and
# so sees TinyORM and Qt.
target_include_directories(pychron_dvc_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/libs/dvc/src"
  "${PROJECT_SOURCE_DIR}/tests/persistence"
  "${PROJECT_SOURCE_DIR}/libs/persistence/src")
target_compile_definitions(pychron_dvc_tests PRIVATE
  PYCHRON_DVC_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures" QT_NO_KEYWORDS)
target_link_libraries(pychron_dvc_tests PRIVATE pychron::dvc pychron::ingest pychron::persistence
  nlohmann_json::nlohmann_json TinyOrm::TinyOrm Qt6::Core Qt6::Sql GTest::gtest_main)
pychron_set_warnings(pychron_dvc_tests)
gtest_discover_tests(pychron_dvc_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)
