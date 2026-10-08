# One test_<component>.cpp per earthbank component. Globbed so parallel units
# only add files. The mapping's fixtures are built on the processing tests'
# (tests/processing/fixtures.hpp), included as "processing/fixtures.hpp".
# test_ooxml.cpp tests an internal unit: it includes "ooxml.hpp" from the
# library's src/ and reads the vendor templates where the library keeps them.
file(GLOB PYCHRON_EARTHBANK_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

add_executable(pychron_earthbank_tests ${PYCHRON_EARTHBANK_TESTS})
target_include_directories(pychron_earthbank_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_include_directories(pychron_earthbank_tests PRIVATE ${PROJECT_SOURCE_DIR}/libs/earthbank/src)
target_compile_definitions(pychron_earthbank_tests PRIVATE
  PYCHRON_EARTHBANK_TEMPLATE_DIR="${PROJECT_SOURCE_DIR}/libs/earthbank/templates")
target_link_libraries(pychron_earthbank_tests PRIVATE pychron::earthbank pychron::processing pychron::reduction
                                                      GTest::gtest_main)
pychron_set_warnings(pychron_earthbank_tests)
gtest_discover_tests(pychron_earthbank_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
