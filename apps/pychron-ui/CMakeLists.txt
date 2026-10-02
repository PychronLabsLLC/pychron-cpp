# Qt6 M1 status/control window (spec section 10). The UI never touches
# drivers or transports: it links pychron::systems only and talks to the core
# through ExtractionLine (commands in) and SignalBus (state out).
find_package(Qt6 REQUIRED COMPONENTS Widgets PrintSupport)

set(CMAKE_AUTOMOC ON)

# Everything except main() lives in a static library so tests/ui can drive
# the same widgets under Qt Test.
add_library(pychron_ui_lib STATIC
  src/core_bridge.cpp
  src/canvas_items.cpp
  src/canvas_view.cpp
  src/log_dock.cpp
  src/log_model.cpp
  src/log_filter_proxy.cpp
  src/alarm_dock.cpp
  src/health_bar.cpp
  src/main_window.cpp
  src/core_bridge.hpp
  src/canvas_items.hpp
  src/canvas_view.hpp
  src/log_dock.hpp
  src/log_model.hpp
  src/log_filter_proxy.hpp
  src/alarm_dock.hpp
  src/health_bar.hpp
  src/main_window.hpp
)
add_library(pychron::ui ALIAS pychron_ui_lib)

target_include_directories(pychron_ui_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pychron_ui_lib PUBLIC pychron::systems Qt6::Widgets qcustomplot)
pychron_set_warnings(pychron_ui_lib)

add_executable(pychron-ui src/main.cpp)
target_link_libraries(pychron-ui PRIVATE pychron_ui_lib)
# spectrometer sim drivers register from static initializers nothing references
pychron_link_drivers(pychron-ui pychron_sim)
target_compile_definitions(pychron-ui PRIVATE
  PYCHRON_EXAMPLE_CONFIGS_DIR="${PROJECT_SOURCE_DIR}/configs/examples")
pychron_set_warnings(pychron-ui)
