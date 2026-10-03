# pychron::ingest — the source-agnostic import core (legacy ingestion spec,
# section 2): batch model, id derivation and the BatchWriter that writes
# batches to the DVC store. Public headers are std-only.
if(NOT PYCHRON_PERSISTENCE_ENABLED)
  return()
endif()

add_library(pychron_ingest STATIC
  src/catalog.cpp
  src/ids.cpp
  src/rewrites.cpp
  src/tz.cpp
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

# Time zones: std::chrono's tzdb where the standard library has it, otherwise
# Howard Hinnant's date (cmake/PychronDependencies.cmake). Only tz.cpp knows.
if(PYCHRON_HAVE_STD_TZDB)
  target_compile_definitions(pychron_ingest PRIVATE PYCHRON_HAVE_STD_TZDB=1)
else()
  target_compile_definitions(pychron_ingest PRIVATE PYCHRON_HAVE_STD_TZDB=0)
  target_link_libraries(pychron_ingest PRIVATE date::date-tz)
endif()
