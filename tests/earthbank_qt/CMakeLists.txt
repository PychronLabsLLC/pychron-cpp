# The HTTPS client against a QTcpServer on 127.0.0.1, started in the test:
# nothing here, or anywhere in the tree's tests, reaches a host on the
# internet. Built with libs/earthbank_qt, so only where Qt6 Network is.
if(NOT PYCHRON_EARTHBANK_NETWORK_ENABLED)
  return()
endif()

# Certificates for the TLS tests: a key and a self-signed certificate made
# here, at configure time, in the build tree, with the openssl program. No key
# is committed. Without openssl (or if it fails) the tests that need them skip.
# `ip`: named for 127.0.0.1, which the client asks for. `name`: named for
# earthbank.test only, so that the name does not match the one asked for.
find_program(PYCHRON_OPENSSL openssl)
set(PYCHRON_TLS_DIR "${CMAKE_CURRENT_BINARY_DIR}/tls")
set(PYCHRON_TLS_READY OFF)
if(PYCHRON_OPENSSL)
  file(MAKE_DIRECTORY "${PYCHRON_TLS_DIR}")
  set(_ok TRUE)
  foreach(_which ip name)
    if(_which STREQUAL "ip")
      set(_alt "IP.1 = 127.0.0.1")
      set(_cn "127.0.0.1")
    else()
      set(_alt "DNS.1 = earthbank.test")
      set(_cn "earthbank.test")
    endif()
    file(WRITE "${PYCHRON_TLS_DIR}/${_which}.cnf"
"[req]
distinguished_name = dn
x509_extensions = ext
prompt = no
[dn]
CN = ${_cn}
[ext]
subjectAltName = @alt
[alt]
${_alt}
")
    if(NOT EXISTS "${PYCHRON_TLS_DIR}/${_which}.crt")
      execute_process(
        COMMAND "${PYCHRON_OPENSSL}" req -x509 -newkey rsa:2048 -nodes -days 30
                -keyout "${_which}.key" -out "${_which}.crt" -config "${_which}.cnf"
        WORKING_DIRECTORY "${PYCHRON_TLS_DIR}"
        RESULT_VARIABLE _result OUTPUT_QUIET ERROR_QUIET)
      if(NOT _result EQUAL 0)
        set(_ok FALSE)
      endif()
    endif()
  endforeach()
  if(_ok)
    set(PYCHRON_TLS_READY ON)
  endif()
endif()
message(STATUS "pychron: earthbank_qt TLS tests: ${PYCHRON_TLS_READY}")

add_executable(pychron_earthbank_qt_tests test_qt_http_client.cpp)
target_compile_definitions(pychron_earthbank_qt_tests PRIVATE QT_NO_KEYWORDS)
target_link_libraries(pychron_earthbank_qt_tests PRIVATE pychron::earthbank_qt pychron::earthbank pychron::core
                                                         Qt6::Core Qt6::Network GTest::gtest_main)
if(PYCHRON_TLS_READY)
  target_compile_definitions(pychron_earthbank_qt_tests PRIVATE PYCHRON_TEST_TLS_DIR="${PYCHRON_TLS_DIR}")
endif()
pychron_set_warnings(pychron_earthbank_qt_tests)
gtest_discover_tests(pychron_earthbank_qt_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)

# The live test (test_live.cpp): an upload to a real EarthBank, skipped unless
# the four PYCHRON_TEST_EARTHBANK_* variables are set, which nothing in the
# tree or in CI does. A program of its own so that it has a label of its own:
# `ctest -LE live` leaves it out, `ctest -L live` runs only it. Its steps are
# tests/earthbank's (live_round_trip.hpp), built on tests/processing's fixtures.
add_executable(pychron_earthbank_live_tests test_live.cpp)
target_include_directories(pychron_earthbank_live_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/tests/earthbank ${PROJECT_SOURCE_DIR}/tests)
target_compile_definitions(pychron_earthbank_live_tests PRIVATE QT_NO_KEYWORDS)
target_link_libraries(pychron_earthbank_live_tests PRIVATE pychron::earthbank_qt pychron::earthbank pychron::processing
                                                           pychron::reduction pychron::core GTest::gtest_main)
pychron_set_warnings(pychron_earthbank_live_tests)
gtest_discover_tests(pychron_earthbank_live_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 600 LABELS live)
