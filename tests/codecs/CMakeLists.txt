add_executable(pychron_codecs_tests
  test_maxigauge.cpp
)

target_link_libraries(pychron_codecs_tests PRIVATE pychron::codecs GTest::gtest_main)
pychron_set_warnings(pychron_codecs_tests)

gtest_discover_tests(pychron_codecs_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
