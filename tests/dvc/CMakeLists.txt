# One test_<component>.cpp per dvc component. The layout tests read the real
# legacy files under fixtures/ (see fixtures/README.md) and need no store.
if(NOT PYCHRON_PERSISTENCE_ENABLED)
  return()
endif()

file(GLOB PYCHRON_DVC_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

add_executable(pychron_dvc_tests ${PYCHRON_DVC_TESTS})
# test_legacy_json.cpp tests the private reader, so it sees src/ and the JSON
# library.
target_include_directories(pychron_dvc_tests PRIVATE "${PROJECT_SOURCE_DIR}/libs/dvc/src")
target_compile_definitions(pychron_dvc_tests PRIVATE
  PYCHRON_DVC_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
target_link_libraries(pychron_dvc_tests PRIVATE pychron::dvc nlohmann_json::nlohmann_json GTest::gtest_main)
pychron_set_warnings(pychron_dvc_tests)
gtest_discover_tests(pychron_dvc_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)
