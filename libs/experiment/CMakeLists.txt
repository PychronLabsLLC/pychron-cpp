# pychron::experiment. Sources are globbed so units landing in parallel only add
# files under src/ and include/pychron/experiment/ and never edit this list.
file(GLOB_RECURSE PYCHRON_EXPERIMENT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

# src/metrics/ reports runs and queues to the Prometheus registry; without
# libs/metrics (PYCHRON_METRICS=OFF) it is left out.
if(NOT TARGET pychron_metrics)
  list(FILTER PYCHRON_EXPERIMENT_SOURCES EXCLUDE REGEX "/src/metrics/")
endif()

add_library(pychron_experiment STATIC ${PYCHRON_EXPERIMENT_SOURCES})
add_library(pychron::experiment ALIAS pychron_experiment)

target_include_directories(pychron_experiment PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(pychron_experiment PUBLIC cxx_std_20)
target_link_libraries(pychron_experiment PUBLIC pychron::systems pychron::scripting pychron::reduction pychron::laser pychron::devices pychron::core)

if(TARGET pychron_metrics)
  target_link_libraries(pychron_experiment PUBLIC pychron::metrics)
  target_compile_definitions(pychron_experiment PUBLIC PYCHRON_EXPERIMENT_HAS_METRICS=1)
endif()

pychron_set_warnings(pychron_experiment)
