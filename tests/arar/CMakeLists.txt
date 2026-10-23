# The Ar-Ar suites: reduction step functions, reduce(), decay, flux and the
# golden vectors under golden/ (spec 9.4). Globbed so parallel units only add
# files.
file(GLOB PYCHRON_ARAR_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

if(PYCHRON_ARAR_TESTS)
  add_executable(pychron_arar_tests ${PYCHRON_ARAR_TESTS})
  target_link_libraries(pychron_arar_tests PRIVATE pychron::arar_reduction GTest::gtest_main)
  # golden_json.hpp, the golden reader shared with tests/reduction.
  target_include_directories(pychron_arar_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
  target_compile_definitions(pychron_arar_tests PRIVATE
    PYCHRON_EXAMPLE_CONFIGS_DIR="${PROJECT_SOURCE_DIR}/configs/examples"
    PYCHRON_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data"
    PYCHRON_REDUCTION_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")
  pychron_set_warnings(pychron_arar_tests)
  # -ffp-contract=off (unfused test arithmetic) comes PUBLIC from pychron::arar_reduction.
  gtest_discover_tests(pychron_arar_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
endif()
