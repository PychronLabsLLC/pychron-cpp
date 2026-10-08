# The asio::asio target, for the libraries that speak to a socket
# (libs/transport, libs/metrics). Included by each; defined once.
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
