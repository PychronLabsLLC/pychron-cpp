# Qt Test smoke suites for apps/pychron-ui (BUILD_UI only). Each test_*.cpp is
# one QTEST_MAIN executable run headless on the offscreen platform against the
# example line with every transport simulated.
find_package(Qt6 REQUIRED COMPONENTS Test Widgets)

set(CMAKE_AUTOMOC ON)

file(GLOB PYCHRON_UI_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")
# The Publish window exists only with the store (apps/pychron-ui, PYCHRON_UI_HAS_STORE).
if(NOT TARGET pychron_publish OR NOT TARGET pychron_processing_store)
  list(FILTER PYCHRON_UI_TESTS EXCLUDE REGEX "test_publish[^/]*\\.cpp$")
endif()

foreach(src IN LISTS PYCHRON_UI_TESTS)
  get_filename_component(name ${src} NAME_WE)
  set(target pychron_ui_${name})
  add_executable(${target} ${src} ui_fixture.hpp)
  # tests/processing: flux_seed.hpp, the seeded level of the flux tests;
  # tests/support: temp_path.hpp, the fixtures' scratch names
  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/tests/processing
    ${PROJECT_SOURCE_DIR}/tests/support)
  target_link_libraries(${target} PRIVATE pychron::ui Qt6::Test)
  # the EarthBank suites talk to the service in memory (tests/earthbank)
  if(name MATCHES "^test_earthbank")
    target_link_libraries(${target} PRIVATE pychron::earthbank_fake)
    # tests/earthbank/analyses.hpp builds on "processing/fixtures.hpp"
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/tests)
  endif()
  # the Publish window's suite seeds a store as the release tests do
  # (tests/publish/release_seed.hpp, over tests/persistence/store_fixture.hpp)
  if(name MATCHES "^test_publish")
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/tests ${PROJECT_SOURCE_DIR}/tests/publish
      "${PROJECT_SOURCE_DIR}/libs/persistence/src")
    target_compile_definitions(${target} PRIVATE QT_NO_KEYWORDS)
    target_link_libraries(${target} PRIVATE TinyOrm::TinyOrm Qt6::Sql GTest::gtest)
  endif()
  target_compile_definitions(${target} PRIVATE
    PYCHRON_EXAMPLE_CONFIGS_DIR="${PROJECT_SOURCE_DIR}/configs/examples"
    PYCHRON_SYSTEMS_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/systems/data"
    PYCHRON_UI_SOURCE_DIR="${PROJECT_SOURCE_DIR}/apps/pychron-ui/src")
  # spectrometer sim drivers register from static initializers nothing references
  pychron_link_drivers(${target} pychron_sim)
  pychron_set_warnings(${target})
  add_test(NAME ui.${name} COMMAND ${target})
  # The longest (the experiment window's) runs its queues in simulated time
  # paced at 400x (VirtualClock), which holds whatever a sleep costs on the
  # runner; the rest is real waiting for the UI, slower on a loaded one. It
  # takes 155 to 170 s on CI's macOS runner, and more on a slow day.
  set_tests_properties(ui.${name} PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    TIMEOUT 300)
endforeach()
