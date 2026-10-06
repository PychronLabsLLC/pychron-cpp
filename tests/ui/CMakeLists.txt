# Qt Test smoke suites for apps/pychron-ui (BUILD_UI only). Each test_*.cpp is
# one QTEST_MAIN executable run headless on the offscreen platform against the
# example line with every transport simulated.
find_package(Qt6 REQUIRED COMPONENTS Test Widgets)

set(CMAKE_AUTOMOC ON)

file(GLOB PYCHRON_UI_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

foreach(src IN LISTS PYCHRON_UI_TESTS)
  get_filename_component(name ${src} NAME_WE)
  set(target pychron_ui_${name})
  add_executable(${target} ${src} ui_fixture.hpp)
  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${target} PRIVATE pychron::ui Qt6::Test)
  target_compile_definitions(${target} PRIVATE
    PYCHRON_EXAMPLE_CONFIGS_DIR="${PROJECT_SOURCE_DIR}/configs/examples"
    PYCHRON_UI_SOURCE_DIR="${PROJECT_SOURCE_DIR}/apps/pychron-ui/src")
  # spectrometer sim drivers register from static initializers nothing references
  pychron_link_drivers(${target} pychron_sim)
  pychron_set_warnings(${target})
  add_test(NAME ui.${name} COMMAND ${target})
  # The longest (the experiment window's) takes 12 s here and five times that
  # on the macOS runners, where a millisecond's sleep is nine: simulated time
  # (ClockPump) runs that much slower there.
  set_tests_properties(ui.${name} PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    TIMEOUT 180)
endforeach()
