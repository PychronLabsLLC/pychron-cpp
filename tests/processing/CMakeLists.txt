# One test_<component>.cpp per processing component. The record-source suite
# and the store-source suite need the experiment and persistence libraries and
# build only when their adapters exist.
file(GLOB PYCHRON_PROCESSING_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")
list(FILTER PYCHRON_PROCESSING_TESTS EXCLUDE REGEX "test_(record|store)_source\\.cpp$")

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

if(TARGET pychron_processing_store)
  add_executable(pychron_processing_store_tests test_store_source.cpp)
  target_link_libraries(pychron_processing_store_tests PRIVATE pychron::processing_store GTest::gtest_main)
  pychron_set_warnings(pychron_processing_store_tests)
  gtest_discover_tests(pychron_processing_store_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)
endif()
