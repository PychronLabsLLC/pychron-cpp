# The laser system: tray maps, stage calibration and the facade that turns a
# hole name into stage millimetres for any extraction-device driver
# (docs/superpowers/specs/2026-10-04-laser-system-design.md). No Qt, no OpenCV.
file(GLOB PYCHRON_LASER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(pychron_laser STATIC ${PYCHRON_LASER_SOURCES})
add_library(pychron::laser ALIAS pychron_laser)

target_include_directories(pychron_laser PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(pychron_laser PUBLIC cxx_std_20)
target_link_libraries(pychron_laser PUBLIC pychron::devices pychron::vision pychron::core)

pychron_set_warnings(pychron_laser)
