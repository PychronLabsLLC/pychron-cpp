# pychron::sim. Sources are globbed so units landing in parallel only add
# files under src/ and include/pychron/sim/ and never edit this list.
file(GLOB_RECURSE PYCHRON_SIM_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(pychron_sim STATIC ${PYCHRON_SIM_SOURCES})
add_library(pychron::sim ALIAS pychron_sim)

target_include_directories(pychron_sim PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(pychron_sim PUBLIC cxx_std_20)
target_link_libraries(pychron_sim PUBLIC pychron::devices pychron::transport pychron::core)

pychron_set_warnings(pychron_sim)
