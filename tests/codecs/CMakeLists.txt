# One test_<vendor>.cpp per vendor codec (see libs/codecs/CONVENTIONS.md);
# spectrometer codecs live under spectrometer/.
file(GLOB PYCHRON_CODEC_TESTS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/spectrometer/test_*.cpp")

add_executable(pychron_codecs_tests ${PYCHRON_CODEC_TESTS})

target_link_libraries(pychron_codecs_tests PRIVATE pychron::codecs nlohmann_json::nlohmann_json GTest::gtest_main)
pychron_set_warnings(pychron_codecs_tests)

gtest_discover_tests(pychron_codecs_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)

target_compile_definitions(pychron_codecs_tests PRIVATE
  PYCHRON_TRACES_DIR="${PROJECT_SOURCE_DIR}/tests/traces"
  PYCHRON_QTEGRA_CODEC_SOURCE="${PROJECT_SOURCE_DIR}/libs/codecs/src/thermo_qtegra.cpp")
