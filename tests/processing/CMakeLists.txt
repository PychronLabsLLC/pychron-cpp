# One test_<component>.cpp per processing component. The record-source suite
# needs the experiment library and builds only when the adapter exists.
file(GLOB PYCHRON_PROCESSING_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")
list(FILTER PYCHRON_PROCESSING_TESTS EXCLUDE REGEX "test_record_source\\.cpp$")

add_executable(pychron_processing_tests ${PYCHRON_PROCESSING_TESTS})
target_include_directories(pychron_processing_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pychron_processing_tests PRIVATE pychron::processing GTest::gtest_main)
pychron_set_warnings(pychron_processing_tests)
gtest_discover_tests(pychron_processing_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)

if(TARGET pychron_processing_records)
  add_executable(pychron_processing_records_tests test_record_source.cpp)
  target_link_libraries(pychron_processing_records_tests PRIVATE pychron::processing_records GTest::gtest_main)
  pychron_set_warnings(pychron_processing_records_tests)
  gtest_discover_tests(pychron_processing_records_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
endif()
