# pychron::vision: frames, image kernel, target finding. Depends on core only.
if(NOT PYCHRON_VISION)
  return()
endif()

file(GLOB_RECURSE PYCHRON_VISION_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(pychron_vision STATIC ${PYCHRON_VISION_SOURCES})
add_library(pychron::vision ALIAS pychron_vision)

target_include_directories(pychron_vision PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(pychron_vision PUBLIC cxx_std_20)
target_link_libraries(pychron_vision PUBLIC pychron::core)

pychron_set_warnings(pychron_vision)
