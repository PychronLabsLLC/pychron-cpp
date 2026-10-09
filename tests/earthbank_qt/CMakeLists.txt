# The HTTPS client against a QTcpServer on 127.0.0.1, started in the test:
# nothing here, or anywhere in the tree's tests, reaches a host on the
# internet. Built with libs/earthbank_qt, so only where Qt6 Network is.
if(NOT PYCHRON_EARTHBANK_NETWORK_ENABLED)
  return()
endif()

add_executable(pychron_earthbank_qt_tests test_qt_http_client.cpp)
target_compile_definitions(pychron_earthbank_qt_tests PRIVATE QT_NO_KEYWORDS)
target_link_libraries(pychron_earthbank_qt_tests PRIVATE pychron::earthbank_qt pychron::earthbank pychron::core
                                                         Qt6::Core Qt6::Network GTest::gtest_main)
pychron_set_warnings(pychron_earthbank_qt_tests)
gtest_discover_tests(pychron_earthbank_qt_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
