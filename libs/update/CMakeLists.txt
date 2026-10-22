# pychron::update: telling the user that a newer release exists. Reads
# versions and GitHub's "latest release" answer. Qt-free and without network
# code: what it asks goes through pychron::http::IHttpClient (libs/core),
# which a program gives it. Sources are globbed so units landing in parallel
# only add files.
file(GLOB PYCHRON_UPDATE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(pychron_update STATIC ${PYCHRON_UPDATE_SOURCES})
add_library(pychron::update ALIAS pychron_update)

target_include_directories(pychron_update PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(pychron_update PUBLIC cxx_std_20)
target_link_libraries(pychron_update PUBLIC pychron::core PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(pychron_update PRIVATE PYCHRON_UPDATE_USER_AGENT="pychron-cpp/${PROJECT_VERSION}")
pychron_set_warnings(pychron_update)
