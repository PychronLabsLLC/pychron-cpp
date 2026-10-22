add_executable(pychron_core_tests
  test_expected.cpp
  test_error.cpp
  test_clock.cpp
  test_virtual_clock.cpp
  test_clock_mutex.cpp
  test_time_series_ring.cpp
  test_signal_bus.cpp
  test_logger.cpp
  test_log_match.cpp
  test_log_hub.cpp
  test_scheduler.cpp
  test_host_name.cpp
  test_process.cpp
  test_user_file.cpp
  test_number.cpp
  test_path_text.cpp
  test_sha256.cpp
  test_sha1.cpp
  test_calendar.cpp
  test_config_loader.cpp
  test_config_validate.cpp
  test_config_override.cpp
  test_local_file.cpp
  test_logging_file.cpp
  test_log_dir.cpp
  test_http.cpp
)

target_link_libraries(pychron_core_tests PRIVATE pychron::core GTest::gtest_main)
# virtual_time.hpp
target_include_directories(pychron_core_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
pychron_set_warnings(pychron_core_tests)

gtest_discover_tests(pychron_core_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)

# tools/quality_check.py (static analysis of a branch's changes) is tested with
# the standard library's unittest; the analysers themselves are not run.
find_package(Python3 3.11 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME tools.quality_check
    COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s "${PROJECT_SOURCE_DIR}/tools/tests" -p "test_quality_check.py"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(tools.quality_check PROPERTIES TIMEOUT 60)
  # tools/compiler_sweep.py --changed: which sources a branch's changes reach,
  # from Ninja's dependency log; no compiler is run.
  add_test(NAME tools.compiler_sweep
    COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s "${PROJECT_SOURCE_DIR}/tools/tests" -p "test_compiler_sweep.py"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(tools.compiler_sweep PROPERTIES TIMEOUT 60)
  # tools/agent_bench (the agent toolchain's ledger and fixed set): its tests
  # start no agent; the runner is given a repository made in a temporary
  # directory and a stand-in for the agent.
  add_test(NAME tools.agent_bench
    COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s "${PROJECT_SOURCE_DIR}/tools/tests" -p "test_agent_bench.py"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(tools.agent_bench PROPERTIES TIMEOUT 120)
endif()
