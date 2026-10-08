include(PychronAsio)

add_library(pychron_transport
  src/bytes.cpp
  src/read_spec.cpp
  src/health.cpp
  src/link_transport.cpp
  src/queued_transport.cpp
  src/sim_transport.cpp
  src/trace.cpp
  src/trace_recorder.cpp
  src/asio_stream.cpp
  src/serial_transport.cpp
  src/tcp_transport.cpp
  src/udp_transport.cpp
  src/factory.cpp
)
add_library(pychron::transport ALIAS pychron_transport)

target_include_directories(pychron_transport PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_include_directories(pychron_transport PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(pychron_transport PUBLIC cxx_std_20)
target_link_libraries(pychron_transport
  PUBLIC pychron::core Threads::Threads
  PRIVATE asio::asio)

if(WIN32)
  # asio pulls in <windows.h>; keep its min/max macros out of our code.
  target_compile_definitions(pychron_transport PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

pychron_set_warnings(pychron_transport)
