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
# nlohmann_json reads the executor's state file back (Executor::read_state) and writes
# the resolved-plan golden form (src/plan/plan_json.cpp); not in public headers.
target_link_libraries(pychron_experiment PRIVATE nlohmann_json::nlohmann_json)

if(TARGET pychron_metrics)
  target_link_libraries(pychron_experiment PUBLIC pychron::metrics)
  target_compile_definitions(pychron_experiment PUBLIC PYCHRON_EXPERIMENT_HAS_METRICS=1)
endif()

pychron_set_warnings(pychron_experiment)

# Turns a finished record into what the DVC store ingests. Its own library so
# that pychron::experiment never depends on persistence; it exists only when
# persistence is built.
if(TARGET pychron_persistence)
  file(GLOB PYCHRON_EXPERIMENT_STORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/adapters/store/src/*.cpp")
  add_library(pychron_experiment_store STATIC ${PYCHRON_EXPERIMENT_STORE_SOURCES})
  add_library(pychron::experiment_store ALIAS pychron_experiment_store)
  target_include_directories(pychron_experiment_store PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/adapters/store/include>
    $<INSTALL_INTERFACE:include>
  )
  target_link_libraries(pychron_experiment_store PUBLIC pychron::experiment pychron::persistence
    PRIVATE nlohmann_json::nlohmann_json)
  target_compile_definitions(pychron_experiment_store PUBLIC PYCHRON_EXPERIMENT_HAS_STORE=1)
  pychron_set_warnings(pychron_experiment_store)
endif()
