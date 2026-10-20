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
  src/database_choice.cpp
  src/core_bridge.cpp
  src/cryo_dock.cpp
  src/heater_dock.cpp
  src/canvas_items.cpp
  src/canvas_state.cpp
  src/canvas_view.cpp
  src/dock_layouts.cpp
  src/log_dock.cpp
  src/log_model.cpp
  src/log_filter_proxy.cpp
  src/alarm_dock.cpp
  src/health_bar.cpp
  src/main_window.cpp
  src/no_database_persister.cpp
  src/strip_chart_model.cpp
  src/intensities_model.cpp
  src/spectrometer_bridge.cpp
  src/strip_chart_view.cpp
  src/spectrometer_window.cpp
  src/experiment_bridge.cpp
  src/queue_table_model.cpp
  src/queue_views.cpp
  src/resume_offer.cpp
  src/recent_queues.cpp
  src/executor_pane.cpp
  src/timeline.cpp
  src/evolutions_view.cpp
  src/experiment_window.cpp
  src/run_factory_panel.cpp
  src/measurement_panel.cpp
  src/script_highlighter.cpp
  src/code_editor.cpp
  src/script_editor_window.cpp
  src/rehearsal_window.cpp
  src/conditional_table_model.cpp
  src/conditional_form.cpp
  src/conditionals_editor_window.cpp
  src/field_table_model.cpp
  src/field_table_editor_window.cpp
  src/field_table_plot.cpp
  src/field_table_dialogs.cpp
  src/scene_view.cpp
  src/flow_layout.cpp
  src/options_editor.cpp
  src/processing_bridge.cpp
  src/analysis_table_model.cpp
  src/facet_box.cpp
  src/row_colors.cpp
  src/data_browser_window.cpp
  src/selection_list.cpp
  src/data_icons.cpp
  src/recall_window.cpp
  src/history_chart.cpp
  src/gauge_popup.cpp
  src/gauge_window.cpp
  src/reading_history.cpp
  src/peak_center_scale.cpp
  src/peak_center_view.cpp
  src/rise_rate.cpp
  src/rise_rate_panel.cpp
  src/figure_window.cpp
  src/reference_fit_window.cpp
  src/reference_series.cpp
  src/reference_series_dock.cpp
  src/preset_bar.cpp
  src/isotope_evolution_window.cpp
  src/theme.cpp
  src/plot_style.cpp
  src/data_workspace.cpp
  src/data_main_window.cpp
  src/data_source_page.cpp
  src/question_editor.cpp
  src/setup_wizard.cpp
  src/installations_dialog.cpp
  src/setup_support.cpp
  src/brand.cpp
  src/menu_hub.cpp
  src/shortcuts.cpp
  src/command_palette.cpp
  src/line_settings.cpp
  src/logging_page.cpp
  src/metrics_page.cpp
  src/preferences.cpp
  src/preferences_dialog.cpp
  src/earthbank_worker.cpp
  src/earthbank_preferences_page.cpp
  src/earthbank_package_dialog.cpp
  src/earthbank_publish_dialog.cpp
  src/laser_bridge.cpp
  src/tray_view.cpp
  src/camera_view.cpp
  src/laser_window.cpp
  src/pattern_maker_window.cpp
  src/command_line.hpp
  src/database_choice.hpp
  src/core_bridge.hpp
  src/cryo_dock.hpp
  src/heater_dock.hpp
  src/canvas_items.hpp
  src/canvas_state.hpp
  src/canvas_view.hpp
  src/dock_layouts.hpp
  src/log_dock.hpp
  src/log_model.hpp
  src/log_filter_proxy.hpp
  src/alarm_dock.hpp
  src/health_bar.hpp
  src/main_window.hpp
  src/no_database_persister.hpp
  src/strip_chart_model.hpp
  src/intensities_model.hpp
  src/spectrometer_bridge.hpp
  src/strip_chart_view.hpp
  src/spectrometer_window.hpp
  src/experiment_bridge.hpp
  src/queue_table_model.hpp
  src/queue_views.hpp
  src/resume_offer.hpp
  src/executor_pane.hpp
  src/timeline.hpp
  src/evolutions_view.hpp
  src/experiment_window.hpp
  src/identifier_source.hpp
  src/run_factory_panel.hpp
  src/measurement_panel.hpp
  src/script_highlighter.hpp
  src/code_editor.hpp
  src/rehearsal_window.hpp
  src/script_editor_window.hpp
  src/conditional_table_model.hpp
  src/conditional_form.hpp
  src/conditionals_editor_window.hpp
  src/field_table_model.hpp
  src/field_table_editor_window.hpp
  src/field_table_plot.hpp
  src/field_table_dialogs.hpp
  src/scene_view.hpp
  src/flow_layout.hpp
  src/options_editor.hpp
  src/processing_bridge.hpp
  src/analysis_table_model.hpp
  src/facet_box.hpp
  src/row_colors.hpp
  src/data_browser_window.hpp
  src/selection_list.hpp
  src/data_icons.hpp
  src/recall_window.hpp
  src/history_chart.hpp
  src/gauge_popup.hpp
  src/gauge_window.hpp
  src/reading_history.hpp
  src/peak_center_scale.hpp
  src/peak_center_view.hpp
  src/rise_rate.hpp
  src/rise_rate_panel.hpp
  src/figure_window.hpp
  src/reference_fit_window.hpp
  src/reference_series.hpp
  src/reference_series_dock.hpp
  src/preset_bar.hpp
  src/isotope_evolution_window.hpp
  src/theme.hpp
  src/data_workspace.hpp
  src/data_main_window.hpp
  src/data_source_page.hpp
  src/question_editor.hpp
  src/setup_wizard.hpp
  src/installations_dialog.hpp
  src/setup_support.hpp
  src/brand.hpp
  src/menu_hub.hpp
  src/shortcuts.hpp
  src/command_palette.hpp
  src/line_settings.hpp
  src/logging_page.hpp
  src/metrics_page.hpp
  src/preferences.hpp
  src/preferences_dialog.hpp
  src/earthbank_worker.hpp
  src/earthbank_preferences_page.hpp
  src/earthbank_package_dialog.hpp
  src/earthbank_publish_dialog.hpp
  src/laser_bridge.hpp
  src/tray_view.hpp
  src/camera_view.hpp
  src/laser_window.hpp
  src/pattern_maker_window.hpp
)
add_library(pychron::ui ALIAS pychron_ui_lib)

target_include_directories(pychron_ui_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pychron_ui_lib PUBLIC pychron::setup pychron::experiment pychron::systems pychron::processing_records Qt6::Widgets
  qcustomplot)
# Publishing to EarthBank: libs/earthbank says what to ask, libs/earthbank_qt
# asks it over HTTPS. Without Qt6 Network there is no client
# (src/earthbank_no_network.cpp) and the EarthBank pages say so.
target_link_libraries(pychron_ui_lib PUBLIC pychron::earthbank)
if(TARGET pychron_earthbank_qt)
  target_link_libraries(pychron_ui_lib PUBLIC pychron::earthbank_qt)
else()
  target_sources(pychron_ui_lib PRIVATE src/earthbank_no_network.cpp)
endif()
pychron_set_warnings(pychron_ui_lib)
target_compile_definitions(pychron_ui_lib PRIVATE PYCHRON_VERSION="${PROJECT_VERSION}")
# --db browses the DVC store; it needs the persistence library.
if(TARGET pychron_processing_store)
  target_link_libraries(pychron_ui_lib PUBLIC pychron::processing_store)
  target_compile_definitions(pychron_ui_lib PUBLIC PYCHRON_UI_HAS_STORE=1)
  # Sample and package entry (entry spec, section 9) writes the same store.
  target_sources(pychron_ui_lib PRIVATE
    src/entry_actions.cpp
    src/entry_bridge.cpp
    src/fit_actions.cpp
    src/flux_analysis_model.cpp
    src/flux_model_support.cpp
    src/flux_monitor_model.cpp
    src/flux_unknown_model.cpp
    src/flux_window.cpp
    src/holder_view.cpp
    src/level_grid_model.cpp
    src/level_sheet_pdf.cpp
    src/package_dialogs.cpp
    src/package_tree_delegate.cpp
    src/packages_window.cpp
    src/sample_import_dialog.cpp
    src/sample_table_model.cpp
    src/samples_window.cpp
    src/store_identifier_source.cpp
    src/entry_actions.hpp
    src/store_identifier_source.hpp
    src/entry_bridge.hpp
    src/entry_headers.hpp
    src/fit_actions.hpp
    src/flux_analysis_model.hpp
    src/flux_model_support.hpp
    src/flux_monitor_model.hpp
    src/flux_unknown_model.hpp
    src/flux_window.hpp
    src/holder_view.hpp
    src/level_grid_model.hpp
    src/level_sheet_pdf.hpp
    src/package_dialogs.hpp
    src/package_tree_delegate.hpp
    src/packages_window.hpp
    src/sample_import_dialog.hpp
    src/sample_table_model.hpp
    src/samples_window.hpp)
  target_link_libraries(pychron_ui_lib PUBLIC pychron::entry)
  # Releases of the store's data (data publishing spec, section 6.3): the
  # Publish window, over libs/publish.
  target_sources(pychron_ui_lib PRIVATE
    src/publish_actions.cpp
    src/publish_window.cpp
    src/release_dialog.cpp
    src/publish_actions.hpp
    src/publish_window.hpp
    src/release_dialog.hpp)
  target_link_libraries(pychron_ui_lib PUBLIC pychron::publish)
  # A queue's runs are saved to the same store.
  target_link_libraries(pychron_ui_lib PUBLIC pychron::experiment_store)
endif()

add_executable(pychron-ui src/main.cpp)
target_link_libraries(pychron-ui PRIVATE pychron_ui_lib)
# main.cpp reports the version in pychron_build_info (the metrics endpoint)
target_compile_definitions(pychron-ui PRIVATE PYCHRON_VERSION="${PROJECT_VERSION}")
# spectrometer sim drivers register from static initializers nothing references
pychron_link_drivers(pychron-ui pychron_sim)
pychron_set_warnings(pychron-ui)
