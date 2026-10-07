# One test_<component>.cpp per earthbank component. Globbed so parallel units
# only add files. reduction is linked for the constants presets the vocabulary
# is checked against.
file(GLOB PYCHRON_EARTHBANK_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

add_executable(pychron_earthbank_tests ${PYCHRON_EARTHBANK_TESTS})
target_link_libraries(pychron_earthbank_tests PRIVATE pychron::earthbank pychron::reduction GTest::gtest_main)
pychron_set_warnings(pychron_earthbank_tests)
gtest_discover_tests(pychron_earthbank_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
