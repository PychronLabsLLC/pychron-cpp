# A driver library nothing references by symbol, linked the way drivers are.
add_library(pychron_devices_test_drivers STATIC archived_driver.cpp)
target_link_libraries(pychron_devices_test_drivers PRIVATE pychron::devices)
pychron_set_warnings(pychron_devices_test_drivers)

add_executable(pychron_devices_tests
  test_device.cpp
  test_capabilities.cpp
  test_driver_registry.cpp
  test_reconnect.cpp
  test_codec_conventions.cpp
  test_pfeiffer_maxigauge.cpp
  test_gp_microion.cpp
  test_proxr_relay.cpp
  test_proxr_board_sim.cpp
  test_sim_valves.cpp
  spectrometer/test_roles.cpp
  spectrometer/test_params.cpp
  spectrometer/test_detectors.cpp
  spectrometer/test_conformance_fakes.cpp
  spectrometer/test_thermo_qtegra.cpp
  spectrometer/test_thermo_qtegra_acquire.cpp
  spectrometer/test_thermo_qtegra_conformance.cpp
  spectrometer/test_ngx.cpp
  spectrometer/legacy/test_dac_positioner.cpp
  spectrometer/legacy/test_serial_hv.cpp
  spectrometer/legacy/test_acquirers.cpp
  spectrometer/legacy/test_legacy_conformance.cpp
  spectrometer/legacy/test_sim_legacy_config.cpp
  extraction/test_capability.cpp
  extraction/test_conformance_fake.cpp
  extraction/test_chromium_sim.cpp
  extraction/test_chromium.cpp
)
# spectrometer/ and extraction/ conformance.hpp and fakes
target_include_directories(pychron_devices_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(pychron_devices_tests PRIVATE pychron::devices GTest::gtest_main)
# Committed wire traces, replayed by driver tests.
target_compile_definitions(pychron_devices_tests PRIVATE
  PYCHRON_TRACES_DIR="${PROJECT_SOURCE_DIR}/tests/traces")
pychron_link_drivers(pychron_devices_tests pychron_devices_test_drivers)
# Example configs, driven end to end by spectrometer/legacy.
target_compile_definitions(pychron_devices_tests PRIVATE
  PYCHRON_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/configs/examples")
target_compile_definitions(pychron_devices_tests PRIVATE
  PYCHRON_TRACE_DIR="${PROJECT_SOURCE_DIR}/tests/traces")
pychron_set_warnings(pychron_devices_tests)

gtest_discover_tests(pychron_devices_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
