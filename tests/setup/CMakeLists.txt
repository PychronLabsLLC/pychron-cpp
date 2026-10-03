# One test_<component>.cpp per setup component.
file(GLOB PYCHRON_SETUP_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

if(PYCHRON_SETUP_TESTS)
  add_executable(pychron_setup_tests ${PYCHRON_SETUP_TESTS})
  # asio: test_connection.cpp serves a simulated NGX controller over TCP.
  target_link_libraries(pychron_setup_tests PRIVATE pychron::setup asio::asio GTest::gtest_main)
  target_compile_definitions(pychron_setup_tests PRIVATE
    PYCHRON_EXAMPLE_CONFIGS_DIR="${PROJECT_SOURCE_DIR}/configs/examples"
    PYCHRON_PROFILES_DIR="${PROJECT_SOURCE_DIR}/profiles")
  # the spectrometer sim drivers the installs' simulation mode names
  pychron_link_drivers(pychron_setup_tests pychron_sim)
  pychron_set_warnings(pychron_setup_tests)
  gtest_discover_tests(pychron_setup_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)
endif()

# The install layout: install the programs and shared files into a scratch
# prefix and check the installed elctl finds the profiles there, not in the
# source tree (cmake/PychronInstall.cmake, setup::find_resources).
if(TARGET elctl)
  add_test(NAME Install.ElctlFindsItsInstalledProfiles
    COMMAND ${CMAKE_COMMAND}
      -DBUILD_DIR=${CMAKE_BINARY_DIR}
      -DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/install-test
      -DCONFIG=$<CONFIG>
      -DELCTL_NAME=$<TARGET_FILE_NAME:elctl>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/install_test.cmake)
  set_tests_properties(Install.ElctlFindsItsInstalledProfiles PROPERTIES TIMEOUT 120)
endif()
