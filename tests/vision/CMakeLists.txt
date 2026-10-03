# One test_<component>.cpp per vision component. Globbed so parallel units only add files.
if(NOT PYCHRON_VISION)
  return()
endif()

file(GLOB PYCHRON_VISION_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

if(PYCHRON_VISION_TESTS)
  add_executable(pychron_vision_tests ${PYCHRON_VISION_TESTS})
  target_link_libraries(pychron_vision_tests PRIVATE pychron::vision GTest::gtest_main)
  target_compile_definitions(pychron_vision_tests PRIVATE
    PYCHRON_VISION_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
  pychron_set_warnings(pychron_vision_tests)
  gtest_discover_tests(pychron_vision_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
endif()
