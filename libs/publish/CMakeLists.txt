# pychron::publish: a release of a lab's reduced data (data publishing and
# hosted sharing design): the row tables of a release, its manifest and OGC
# API - Records catalog, the build of the release directory, and the release,
# embargo and registration workflows over the store. Qt-free. Built only with
# the store (persistence) it reads. Apache Arrow (Parquet) is optional and is
# seen by src/parquet_writer.cpp alone; without it make_parquet_writer() is
# null and a build writes everything but data/*.parquet.
if(NOT PYCHRON_PERSISTENCE_ENABLED)
  return()
endif()

file(GLOB PYCHRON_PUBLISH_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(pychron_publish STATIC ${PYCHRON_PUBLISH_SOURCES})
add_library(pychron::publish ALIAS pychron_publish)

target_include_directories(pychron_publish PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(pychron_publish PUBLIC cxx_std_20)
target_link_libraries(pychron_publish PUBLIC pychron::persistence pychron::processing pychron::processing_store
                                            pychron::reduction pychron::core nlohmann_json::nlohmann_json)
pychron_set_warnings(pychron_publish)
