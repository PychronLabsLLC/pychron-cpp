# pychron::setup: install profiles, the site config and doctor (installation
# wizard spec). Qt-free; used by elctl init/doctor and the UI's setup wizard.
file(GLOB_RECURSE PYCHRON_SETUP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(pychron_setup STATIC ${PYCHRON_SETUP_SOURCES})
add_library(pychron::setup ALIAS pychron_setup)

target_include_directories(pychron_setup PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(pychron_setup PUBLIC cxx_std_20)
target_link_libraries(pychron_setup PUBLIC pychron::experiment pychron::systems pychron::core)

# The source tree: where a program that is not installed finds the profiles
# and example configs (setup::find_resources).
target_compile_definitions(pychron_setup PRIVATE PYCHRON_SETUP_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

pychron_set_warnings(pychron_setup)
