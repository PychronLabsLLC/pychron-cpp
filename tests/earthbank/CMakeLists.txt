# One test_<component>.cpp per earthbank component. Globbed so parallel units
# only add files. The mapping's fixtures are built on the processing tests'
# (tests/processing/fixtures.hpp), included as "processing/fixtures.hpp".
# test_ooxml.cpp tests an internal unit, and test_xlsx.cpp reads workbooks back
# with it: they include "ooxml.hpp" from the library's src/ and read the
# vendor templates where the library keeps them.
file(GLOB PYCHRON_EARTHBANK_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

# pychron::earthbank_fake: EarthBank in memory (fake_earthbank.hpp), the only
# one a test of the REST route talks to: the real service is in production and
# has no sandbox. A library of its own so that the tests of the programs
# (apps/elctl/tests, the UI's) link the same one; it brings its include
# directory and where its fixtures are. Nothing under libs/ or apps/*/src may
# depend on it.
add_library(pychron_earthbank_fake STATIC fake_earthbank.cpp)
add_library(pychron::earthbank_fake ALIAS pychron_earthbank_fake)
target_include_directories(pychron_earthbank_fake PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(pychron_earthbank_fake PUBLIC
  PYCHRON_EARTHBANK_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
target_link_libraries(pychron_earthbank_fake PUBLIC pychron::earthbank pychron::core nlohmann_json::nlohmann_json)
pychron_set_warnings(pychron_earthbank_fake)

add_executable(pychron_earthbank_tests ${PYCHRON_EARTHBANK_TESTS})
target_include_directories(pychron_earthbank_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_include_directories(pychron_earthbank_tests PRIVATE ${PROJECT_SOURCE_DIR}/libs/earthbank/src)
target_compile_definitions(pychron_earthbank_tests PRIVATE
  PYCHRON_EARTHBANK_TEMPLATE_DIR="${PROJECT_SOURCE_DIR}/libs/earthbank/templates")
target_link_libraries(pychron_earthbank_tests PRIVATE pychron::earthbank_fake pychron::earthbank pychron::processing
                                                      pychron::reduction GTest::gtest_main)
pychron_set_warnings(pychron_earthbank_tests)
gtest_discover_tests(pychron_earthbank_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
