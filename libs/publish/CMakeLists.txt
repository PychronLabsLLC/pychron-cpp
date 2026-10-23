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
target_link_libraries(pychron_publish PUBLIC pychron::persistence pychron::arar pychron::processing pychron::processing_store
                                            pychron::reduction pychron::core nlohmann_json::nlohmann_json)
pychron_set_warnings(pychron_publish)
# Arrow (PychronDependencies.cmake): parquet_writer.cpp alone sees it; without
# it that file is empty and parquet_stub.cpp gives a null writer.
if(PYCHRON_PUBLISH_ARROW_ENABLED)
  target_link_libraries(pychron_publish PRIVATE ${PYCHRON_PUBLISH_ARROW_LIBS})
  target_compile_definitions(pychron_publish PRIVATE PYCHRON_PUBLISH_ARROW_ENABLED)
endif()
# The licence texts a release ships (LICENSE.txt): installed beside the
# program (cmake/PychronInstall.cmake); an uninstalled build finds them here.
target_compile_definitions(pychron_publish PRIVATE
  PYCHRON_PUBLISH_SOURCE_LICENCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/licences")
