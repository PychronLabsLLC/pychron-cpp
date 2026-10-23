file(GLOB PYCHRON_ELCTL_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

add_executable(pychron_elctl_tests ${PYCHRON_ELCTL_TESTS})
target_link_libraries(pychron_elctl_tests PRIVATE pychron_elctl_lib pychron::update_testing GTest::gtest_main)
pychron_link_drivers(pychron_elctl_tests pychron_sim)
# log_capture.hpp: a process log hub on a temporary folder, read back.
target_include_directories(pychron_elctl_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
target_compile_definitions(pychron_elctl_tests PRIVATE
  PYCHRON_EXAMPLE_CONFIGS_DIR="${PROJECT_SOURCE_DIR}/configs/examples"
  PYCHRON_SYSTEMS_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/systems/data")
# test_import_cmd.cpp builds legacy repositories with the helpers of
# tests/dvc and looks behind the store through its white-box connection, as
# the dvc tests do; without persistence it holds one test, of the stub.
if(TARGET pychron_persistence)
  # TinyORM by its headers only: its library is already linked through persistence.
  # test_earthbank_commands.cpp reads the workbooks back with the library's
  # own reader, "ooxml.hpp" in its src/, as the earthbank tests do.
  target_include_directories(pychron_elctl_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/tests/dvc"
    "${PROJECT_SOURCE_DIR}/tests/processing"
    "${PROJECT_SOURCE_DIR}/libs/persistence/src"
    "${PROJECT_SOURCE_DIR}/libs/earthbank/src"
    $<TARGET_PROPERTY:TinyOrm::TinyOrm,INTERFACE_INCLUDE_DIRECTORIES>)
  target_compile_definitions(pychron_elctl_tests PRIVATE
    PYCHRON_DVC_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/tests/dvc/fixtures" QT_NO_KEYWORDS
    PYCHRON_EARTHBANK_TEMPLATE_DIR="${PROJECT_SOURCE_DIR}/libs/earthbank/templates"
    PYCHRON_EARTHBANK_SOURCE_DIR="${PROJECT_SOURCE_DIR}/libs/earthbank/src"
    $<TARGET_PROPERTY:TinyOrm::TinyOrm,INTERFACE_COMPILE_DEFINITIONS>)
  # dvc, ingest and persistence come with pychron_elctl_lib.
  # earthbank_fake: EarthBank in memory (tests/earthbank), which the account
  # commands are tested against; nothing here reaches the service.
  target_link_libraries(pychron_elctl_tests PRIVATE nlohmann_json::nlohmann_json Qt6::Core Qt6::Sql
    pychron::earthbank_fake)
endif()
pychron_set_warnings(pychron_elctl_tests)
# The import tests build git repositories and stores; under sanitizers on a
# loaded CI machine one can take minutes.
gtest_discover_tests(pychron_elctl_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 300)
