# Qt6 M1 status/control window (spec section 10). The UI never touches
# drivers or transports: it links pychron::systems and pychron::experiment and
# talks to the core through ExtractionLine, Spectrometer and LabSession
# (commands in) and SignalBus (state out). Data windows use pychron::processing
# (Qt-free) through ProcessingBridge.
find_package(Qt6 REQUIRED COMPONENTS Widgets PrintSupport)

set(CMAKE_AUTOMOC ON)

# Everything except main() lives in a static library so tests/ui can drive
# the same widgets under Qt Test.
add_library(pychron_ui_lib STATIC
  src/command_line.cpp
  src/core_bridge.cpp
  src/canvas_items.cpp
  src/canvas_view.cpp
  src/log_dock.cpp
  src/log_model.cpp
  src/log_filter_proxy.cpp
  src/alarm_dock.cpp
  src/health_bar.cpp
  src/main_window.cpp
  src/strip_chart_model.cpp
  src/intensities_model.cpp
  src/spectrometer_bridge.cpp
  src/strip_chart_view.cpp
  src/spectrometer_window.cpp
  src/experiment_bridge.cpp
  src/queue_table_model.cpp
  src/executor_pane.cpp
  src/timeline.cpp
  src/evolutions_view.cpp
  src/experiment_window.cpp
  src/run_factory_panel.cpp
  src/measurement_panel.cpp
  src/script_highlighter.cpp
  src/code_editor.cpp
  src/script_editor_window.cpp
  src/scene_view.cpp
  src/options_editor.cpp
  src/processing_bridge.cpp
  src/analysis_table_model.cpp
  src/data_browser_window.cpp
  src/recall_window.cpp
  src/figure_window.cpp
  src/reference_fit_window.cpp
  src/preset_bar.cpp
  src/theme.cpp
  src/data_workspace.cpp
  src/data_main_window.cpp
  src/setup_wizard.cpp
  src/installations_dialog.cpp
  src/setup_support.cpp
  src/command_line.hpp
  src/core_bridge.hpp
  src/canvas_items.hpp
  src/canvas_view.hpp
  src/log_dock.hpp
  src/log_model.hpp
  src/log_filter_proxy.hpp
  src/alarm_dock.hpp
  src/health_bar.hpp
  src/main_window.hpp
  src/strip_chart_model.hpp
  src/intensities_model.hpp
  src/spectrometer_bridge.hpp
  src/strip_chart_view.hpp
  src/spectrometer_window.hpp
  src/experiment_bridge.hpp
  src/queue_table_model.hpp
  src/executor_pane.hpp
  src/timeline.hpp
  src/evolutions_view.hpp
  src/experiment_window.hpp
  src/run_factory_panel.hpp
  src/measurement_panel.hpp
  src/script_highlighter.hpp
  src/code_editor.hpp
  src/script_editor_window.hpp
  src/scene_view.hpp
  src/options_editor.hpp
  src/processing_bridge.hpp
  src/analysis_table_model.hpp
  src/data_browser_window.hpp
  src/recall_window.hpp
  src/figure_window.hpp
  src/reference_fit_window.hpp
  src/preset_bar.hpp
  src/theme.hpp
  src/data_workspace.hpp
  src/data_main_window.hpp
  src/setup_wizard.hpp
  src/installations_dialog.hpp
  src/setup_support.hpp
)
add_library(pychron::ui ALIAS pychron_ui_lib)

target_include_directories(pychron_ui_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pychron_ui_lib PUBLIC pychron::setup pychron::experiment pychron::systems pychron::processing_records Qt6::Widgets
  qcustomplot)
pychron_set_warnings(pychron_ui_lib)
# --db browses the DVC store; it needs the persistence library.
if(TARGET pychron_processing_store)
  target_link_libraries(pychron_ui_lib PUBLIC pychron::processing_store)
  target_compile_definitions(pychron_ui_lib PUBLIC PYCHRON_UI_HAS_STORE=1)
endif()

add_executable(pychron-ui src/main.cpp)
target_link_libraries(pychron-ui PRIVATE pychron_ui_lib)
# spectrometer sim drivers register from static initializers nothing references
pychron_link_drivers(pychron-ui pychron_sim)
pychron_set_warnings(pychron-ui)
