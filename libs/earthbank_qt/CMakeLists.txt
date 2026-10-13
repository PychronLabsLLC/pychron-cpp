# pychron::earthbank_qt: EarthBank over HTTPS, on Qt6 Network. The only code
# of ours that opens a socket to the service; libs/earthbank says what to ask
# and goes through IHttpClient. Qt is PRIVATE: the public header is std-only.
# Built only when Qt6 Core and Network are found (cmake/PychronDependencies.cmake).
if(NOT PYCHRON_EARTHBANK_NETWORK_ENABLED)
  return()
endif()

add_library(pychron_earthbank_qt STATIC src/qt_http_client.cpp)
add_library(pychron::earthbank_qt ALIAS pychron_earthbank_qt)

target_include_directories(pychron_earthbank_qt PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(pychron_earthbank_qt PUBLIC cxx_std_20)
target_link_libraries(pychron_earthbank_qt PUBLIC pychron::earthbank pychron::core
                                           PRIVATE Qt6::Core Qt6::Network)
target_compile_definitions(pychron_earthbank_qt PRIVATE QT_NO_KEYWORDS
  PYCHRON_EARTHBANK_USER_AGENT="pychron-cpp/${PROJECT_VERSION}")
pychron_set_warnings(pychron_earthbank_qt)
