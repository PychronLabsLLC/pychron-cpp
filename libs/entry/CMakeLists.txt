# pychron::entry — sample and package entry (2026-10-04 entry spec, section
# 6): validation, CSV import, the level-sheet edit model and the identifier
# planner over the DVC store. Qt-free; public headers are std-only.
if(NOT PYCHRON_PERSISTENCE_ENABLED)
  return()
endif()

add_library(pychron_entry STATIC
  src/csv.cpp
  src/export.cpp
  src/holder_import.cpp
  src/identifier_plan.cpp
  src/level_sheet.cpp
  src/names.cpp
  src/package_edit.cpp
  src/positions_import.cpp
  src/sample_fields.cpp
  src/sample_import.cpp
  src/sample_search.cpp
  src/seed.cpp
  src/settings.cpp
)
add_library(pychron::entry ALIAS pychron_entry)

target_include_directories(pychron_entry
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(pychron_entry PUBLIC cxx_std_20)
# dvc for the legacy holder and chronology formats; nlohmann_json for the
# settings and reactors documents. Neither appears in a public header.
target_link_libraries(pychron_entry
  PUBLIC pychron::persistence pychron::core
  PRIVATE pychron::dvc nlohmann_json::nlohmann_json)

pychron_set_warnings(pychron_entry)
