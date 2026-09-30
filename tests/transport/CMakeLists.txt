add_executable(pychron_transport_tests
  test_bytes.cpp
  test_read_spec.cpp
  test_queued_transport.cpp
  test_sim_transport.cpp
  test_trace.cpp
  test_trace_recorder.cpp
  test_tcp_transport.cpp
  test_serial_transport.cpp
  test_factory.cpp
)

target_link_libraries(pychron_transport_tests PRIVATE pychron::transport asio::asio GTest::gtest_main)
if(WIN32)
  target_compile_definitions(pychron_transport_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
pychron_set_warnings(pychron_transport_tests)

gtest_discover_tests(pychron_transport_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
