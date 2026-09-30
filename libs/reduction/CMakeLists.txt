# pychron::reduction. Sources are globbed so units landing in parallel only add
# files under src/ and include/pychron/reduction/ and never edit this list.
file(GLOB_RECURSE PYCHRON_REDUCTION_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(pychron_reduction STATIC ${PYCHRON_REDUCTION_SOURCES})
add_library(pychron::reduction ALIAS pychron_reduction)

target_include_directories(pychron_reduction PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(pychron_reduction PUBLIC cxx_std_20)
target_link_libraries(pychron_reduction PUBLIC pychron::core)

pychron_set_warnings(pychron_reduction)
