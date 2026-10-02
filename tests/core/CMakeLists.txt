add_executable(pychron_core_tests
  test_expected.cpp
  test_error.cpp
  test_clock.cpp
  test_time_series_ring.cpp
  test_signal_bus.cpp
  test_logger.cpp
  test_log_match.cpp
  test_log_hub.cpp
  test_scheduler.cpp
  test_sha256.cpp
  test_config_loader.cpp
  test_config_validate.cpp
  test_config_override.cpp
)

target_link_libraries(pychron_core_tests PRIVATE pychron::core GTest::gtest_main)
pychron_set_warnings(pychron_core_tests)

gtest_discover_tests(pychron_core_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
