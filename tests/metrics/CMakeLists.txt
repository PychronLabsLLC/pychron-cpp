add_executable(pychron_metrics_tests
  test_registry.cpp
  test_render.cpp
  test_server.cpp
  test_scheduler_metrics.cpp
  test_core_exporter.cpp
)

target_link_libraries(pychron_metrics_tests PRIVATE pychron::metrics asio::asio GTest::gtest_main)
target_include_directories(pychron_metrics_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# test_packaging.cpp reads packaging/observability from the source tree.
target_compile_definitions(pychron_metrics_tests PRIVATE PYCHRON_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
if(WIN32)
  target_compile_definitions(pychron_metrics_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
pychron_set_warnings(pychron_metrics_tests)

gtest_discover_tests(pychron_metrics_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
