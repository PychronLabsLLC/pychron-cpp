# pychron::ingest — the source-agnostic import core (legacy ingestion spec,
# section 2): batch model, id derivation and the BatchWriter that writes
# batches to the DVC store. Public headers are std-only.
if(NOT PYCHRON_PERSISTENCE_ENABLED)
  return()
endif()

add_library(pychron_ingest STATIC
  src/catalog.cpp
  src/ids.cpp
  src/writer.cpp
)
add_library(pychron::ingest ALIAS pychron_ingest)

target_include_directories(pychron_ingest
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(pychron_ingest PUBLIC cxx_std_20)
target_link_libraries(pychron_ingest PUBLIC pychron::persistence pychron::core)

pychron_set_warnings(pychron_ingest)
