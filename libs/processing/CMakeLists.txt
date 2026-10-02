# pychron::processing (data browsing and visualization design): analysis model,
# quantities, datasets, options and presets, composable units, scenes. Qt-free;
# depends on core and reduction only. Sources are globbed so parallel units
# only add files.
file(GLOB PYCHRON_PROCESSING_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(pychron_processing STATIC ${PYCHRON_PROCESSING_SOURCES})
add_library(pychron::processing ALIAS pychron_processing)

target_include_directories(pychron_processing PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(pychron_processing PUBLIC cxx_std_20)
target_link_libraries(pychron_processing PUBLIC pychron::reduction pychron::core)
pychron_set_warnings(pychron_processing)

# The AnalysisRecord directory source links the experiment library, so it is a
# separate target: pychron::processing never pulls in the experiment stack.
if(TARGET pychron_experiment)
  file(GLOB PYCHRON_PROCESSING_RECORDS_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/adapters/records/src/*.cpp")
  add_library(pychron_processing_records STATIC ${PYCHRON_PROCESSING_RECORDS_SOURCES})
  add_library(pychron::processing_records ALIAS pychron_processing_records)
  target_include_directories(pychron_processing_records PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/adapters/records/include>
    $<INSTALL_INTERFACE:include>
  )
  target_link_libraries(pychron_processing_records PUBLIC pychron::processing pychron::experiment)
  pychron_set_warnings(pychron_processing_records)
endif()
