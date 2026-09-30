# pychron::scripting. Sources are globbed so units landing in parallel only add
# files under src/ and include/pychron/scripting/ and never edit this list.
# src/python/ holds the embedded-CPython host (PYCHRON_SCRIPTING); src/stub/
# the host that rejects scripted runs when scripting is off.
file(GLOB_RECURSE PYCHRON_SCRIPTING_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
if(PYCHRON_SCRIPTING_ENABLED)
  list(FILTER PYCHRON_SCRIPTING_SOURCES EXCLUDE REGEX "/src/stub/")
else()
  list(FILTER PYCHRON_SCRIPTING_SOURCES EXCLUDE REGEX "/src/python/")
endif()

add_library(pychron_scripting STATIC ${PYCHRON_SCRIPTING_SOURCES})
add_library(pychron::scripting ALIAS pychron_scripting)

target_include_directories(pychron_scripting PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(pychron_scripting PUBLIC cxx_std_20)
target_link_libraries(pychron_scripting PUBLIC pychron::devices pychron::core)

if(PYCHRON_SCRIPTING_ENABLED)
  target_link_libraries(pychron_scripting PRIVATE pybind11::embed)
  target_compile_definitions(pychron_scripting PUBLIC PYCHRON_SCRIPTING_ENABLED=1)
  # pybind11 requires hidden visibility for its own types.
  set_target_properties(pychron_scripting PROPERTIES CXX_VISIBILITY_PRESET hidden)
else()
  target_compile_definitions(pychron_scripting PUBLIC PYCHRON_SCRIPTING_ENABLED=0)
endif()

pychron_set_warnings(pychron_scripting)
