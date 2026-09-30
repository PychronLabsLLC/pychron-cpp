# Managers and facades built on capability interfaces: SwitchManager,
# GaugeScanner, NetworkGraph/canvas model, ExtractionLine facade, and later
# the Spectrometer. Vendor-blind: depends on pychron::devices interfaces only.
#
# Sources are globbed so units landing in parallel only add files under
# src/ and include/pychron/systems/ and never edit this list.
file(GLOB_RECURSE PYCHRON_SYSTEMS_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(pychron_systems STATIC ${PYCHRON_SYSTEMS_SOURCES})
add_library(pychron::systems ALIAS pychron_systems)

target_include_directories(pychron_systems PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(pychron_systems PUBLIC cxx_std_20)
# pychron::sim backs `kind = "sim"` transports in the ExtractionLine facade.
target_link_libraries(pychron_systems PUBLIC pychron::sim pychron::devices pychron::transport pychron::core)

pychron_set_warnings(pychron_systems)
