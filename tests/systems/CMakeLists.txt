# One test_<component>.cpp per systems component. Globbed so parallel units
# only add files here.
file(GLOB PYCHRON_SYSTEMS_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

if(PYCHRON_SYSTEMS_TESTS)
  add_executable(pychron_systems_tests ${PYCHRON_SYSTEMS_TESTS})
  target_link_libraries(pychron_systems_tests PRIVATE pychron::systems GTest::gtest_main)
  # virtual_time.hpp
  target_include_directories(pychron_systems_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
  target_compile_definitions(pychron_systems_tests PRIVATE
    PYCHRON_EXAMPLE_CONFIGS_DIR="${PROJECT_SOURCE_DIR}/configs/examples"
    PYCHRON_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data")
  # spectrometer sim drivers register from static initializers nothing references
  pychron_link_drivers(pychron_systems_tests pychron_sim)
  pychron_set_warnings(pychron_systems_tests)
  gtest_discover_tests(pychron_systems_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
endif()
