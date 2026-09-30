# pychron::scripting. Sources are globbed so units landing in parallel only add
# files under src/ and include/pychron/scripting/ and never edit this list.
file(GLOB_RECURSE PYCHRON_SCRIPTING_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(pychron_scripting STATIC ${PYCHRON_SCRIPTING_SOURCES})
add_library(pychron::scripting ALIAS pychron_scripting)

target_include_directories(pychron_scripting PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(pychron_scripting PUBLIC cxx_std_20)
# Python embedding is added by the scripting_host unit behind PYCHRON_SCRIPTING.
target_link_libraries(pychron_scripting PUBLIC pychron::devices pychron::core)

pychron_set_warnings(pychron_scripting)
