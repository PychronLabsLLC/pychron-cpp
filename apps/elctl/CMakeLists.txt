# elctl: headless CLI for bring-up and scripting (spec section 9.3).
#
# Everything but main() lives in pychron_elctl_lib so the tests under
# tests/ drive the exact command code the binary runs.
file(GLOB PYCHRON_ELCTL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(REMOVE_ITEM PYCHRON_ELCTL_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

add_library(pychron_elctl_lib STATIC ${PYCHRON_ELCTL_SOURCES})
target_include_directories(pychron_elctl_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(pychron_elctl_lib PUBLIC cxx_std_20)
target_link_libraries(pychron_elctl_lib PUBLIC pychron::experiment pychron::systems pychron::devices pychron::transport pychron::core)
pychron_set_warnings(pychron_elctl_lib)

add_executable(elctl src/main.cpp)
target_link_libraries(elctl PRIVATE pychron_elctl_lib)
# sim spectrometer drivers register from static initializers nothing references
pychron_link_drivers(elctl pychron_sim)
pychron_set_warnings(elctl)

# App tests live beside the app: the top-level tests/ loop only covers libs/.
if(BUILD_TESTS)
  enable_testing()
  include(GoogleTest)
  add_subdirectory(tests)
endif()
