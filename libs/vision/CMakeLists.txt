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

# legacy_finder.cpp and opencv_source.cpp are always compiled; each reduces to
# its stub without PYCHRON_VISION_OPENCV_ENABLED. SYSTEM keeps OpenCV's own
# warnings away from -Werror.
if(PYCHRON_VISION_OPENCV_ENABLED)
  target_include_directories(pychron_vision SYSTEM PRIVATE ${OpenCV_INCLUDE_DIRS})
  target_link_libraries(pychron_vision PRIVATE ${OpenCV_LIBS})
  target_compile_definitions(pychron_vision PRIVATE PYCHRON_VISION_OPENCV_ENABLED)
endif()

pychron_set_warnings(pychron_vision)
