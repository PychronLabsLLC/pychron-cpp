# A driver library nothing references by symbol, linked the way drivers are.
add_library(pychron_devices_test_drivers STATIC archived_driver.cpp)
target_link_libraries(pychron_devices_test_drivers PRIVATE pychron::devices)
pychron_set_warnings(pychron_devices_test_drivers)

add_executable(pychron_devices_tests
  test_device.cpp
  test_capabilities.cpp
  test_driver_registry.cpp
  test_codec_conventions.cpp
  test_pfeiffer_maxigauge.cpp
  test_gp_microion.cpp
  test_proxr_relay.cpp
  test_proxr_board_sim.cpp
)

target_link_libraries(pychron_devices_tests PRIVATE pychron::devices GTest::gtest_main)
# Committed wire traces, replayed by driver tests.
target_compile_definitions(pychron_devices_tests PRIVATE
  PYCHRON_TRACES_DIR="${PROJECT_SOURCE_DIR}/tests/traces")
pychron_link_drivers(pychron_devices_tests pychron_devices_test_drivers)
target_compile_definitions(pychron_devices_tests PRIVATE
  PYCHRON_TRACE_DIR="${PROJECT_SOURCE_DIR}/tests/traces")
pychron_set_warnings(pychron_devices_tests)

gtest_discover_tests(pychron_devices_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
