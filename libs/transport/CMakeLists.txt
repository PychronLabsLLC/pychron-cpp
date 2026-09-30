find_package(Threads REQUIRED)

# Standalone asio (header-only). vcpkg provides asio::asio via find_package;
# otherwise fetch a pinned release. asio is a PRIVATE dependency: no public
# transport header includes it.
if(NOT TARGET asio::asio)
  find_package(asio CONFIG QUIET GLOBAL)
endif()
if(NOT TARGET asio::asio)
  include(FetchContent)
  FetchContent_Declare(asio
    URL https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-30-2.tar.gz
    URL_HASH SHA256=755bd7f85a4b269c67ae0ea254907c078d408cce8e1a352ad2ed664d233780e8
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(asio)
  add_library(pychron_asio INTERFACE)
  target_include_directories(pychron_asio SYSTEM INTERFACE "${asio_SOURCE_DIR}/asio/include")
  target_compile_definitions(pychron_asio INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
  target_link_libraries(pychron_asio INTERFACE Threads::Threads)
  if(WIN32)
    target_compile_definitions(pychron_asio INTERFACE _WIN32_WINNT=0x0A00)
    target_link_libraries(pychron_asio INTERFACE ws2_32 mswsock)
  endif()
  add_library(asio::asio ALIAS pychron_asio)
endif()

add_library(pychron_transport
  src/bytes.cpp
  src/read_spec.cpp
  src/health.cpp
  src/queued_transport.cpp
  src/sim_transport.cpp
  src/trace.cpp
  src/trace_recorder.cpp
  src/asio_stream.cpp
  src/serial_transport.cpp
  src/tcp_transport.cpp
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
