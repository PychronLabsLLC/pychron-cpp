# pychron::ingest — the source-agnostic import core (legacy ingestion spec,
# section 2): batch model, id derivation, the BatchWriter that writes
# batches to the DVC store, and the verifier that checks an import. Public
# headers are std-only.
if(NOT PYCHRON_PERSISTENCE_ENABLED)
  return()
endif()

add_library(pychron_ingest STATIC
  src/catalog.cpp
  src/catalog_find.cpp
  src/ids.cpp
  src/late_revision.cpp
  src/rewrites.cpp
  src/tz.cpp
  src/verify.cpp
  src/verify_accounting.cpp
  src/verify_parity.cpp
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
# nlohmann_json reads the JSON the store hands back (provenance and conflict
# detail, the interpreted-age document); it appears in no public header.
target_link_libraries(pychron_ingest
  PUBLIC pychron::persistence pychron::core
  PRIVATE nlohmann_json::nlohmann_json)

pychron_set_warnings(pychron_ingest)

# Time zones: std::chrono's tzdb where the standard library has it, otherwise
# Howard Hinnant's date (cmake/PychronDependencies.cmake). Only tz.cpp knows.
if(PYCHRON_HAVE_STD_TZDB)
  target_compile_definitions(pychron_ingest PRIVATE PYCHRON_HAVE_STD_TZDB=1)
else()
  target_compile_definitions(pychron_ingest PRIVATE PYCHRON_HAVE_STD_TZDB=0)
  target_link_libraries(pychron_ingest PRIVATE date::date-tz)
endif()
