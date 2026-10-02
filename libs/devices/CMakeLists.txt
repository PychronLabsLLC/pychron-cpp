# Device base, capability interfaces, DriverRegistry and (later) the vendor
# drivers that compose a Transport with a codec.
add_library(pychron_devices STATIC
  src/device.cpp
  src/types.cpp
  src/driver_registry.cpp
  src/reconnect.cpp
  src/pfeiffer_maxigauge.cpp
  src/gp_microion.cpp
  src/proxr_relay.cpp
  src/proxr_board_sim.cpp
  src/spectrometer/types.cpp
  src/spectrometer/params.cpp
  src/spectrometer/roles.cpp
  src/spectrometer/detectors.cpp
  src/spectrometer/legacy/polled_acquirer.cpp
  src/spectrometer/legacy/dac_output.cpp
  src/spectrometer/legacy/dac_positioner.cpp
  src/spectrometer/legacy/adc_bank.cpp
  src/spectrometer/legacy/pulse_counter.cpp
  src/spectrometer/legacy/serial_hv.cpp
  src/extraction/capability.cpp
)

target_include_directories(pychron_devices PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(pychron_devices PUBLIC cxx_std_20)
target_link_libraries(pychron_devices PUBLIC pychron::codecs pychron::transport pychron::core)

pychron_set_warnings(pychron_devices)

# pychron_link_drivers(<consumer> <driver-lib>...)
# Drivers register themselves from static initializers (REGISTER_DRIVER) that
# nothing references by symbol, so a plain static link may drop them. Link
# driver libraries whole-archive instead.
function(pychron_link_drivers consumer)
  foreach(lib IN LISTS ARGN)
    target_link_libraries(${consumer} PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,${lib}>")
  endforeach()
endfunction()

# pychron::devices links the library whole-archive so drivers built into it
# are always registered.
add_library(pychron_devices_whole INTERFACE)
target_link_libraries(pychron_devices_whole INTERFACE "$<LINK_LIBRARY:WHOLE_ARCHIVE,pychron_devices>")
add_library(pychron::devices ALIAS pychron_devices_whole)
