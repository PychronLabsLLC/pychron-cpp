include(PychronAsio)

# pychron::metrics: a registry of counters, gauges and histograms, their
# Prometheus text rendering and the HTTP endpoint that serves it. asio is a
# PRIVATE dependency: no public metrics header includes it.
add_library(pychron_metrics
  src/registry.cpp
  src/render.cpp
  src/server.cpp
)
add_library(pychron::metrics ALIAS pychron_metrics)

target_include_directories(pychron_metrics PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_include_directories(pychron_metrics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(pychron_metrics PUBLIC cxx_std_20)
target_link_libraries(pychron_metrics
  PUBLIC pychron::core Threads::Threads
  PRIVATE asio::asio)

if(WIN32)
  # asio pulls in <windows.h>; keep its min/max macros out of our code.
  target_compile_definitions(pychron_metrics PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

pychron_set_warnings(pychron_metrics)
