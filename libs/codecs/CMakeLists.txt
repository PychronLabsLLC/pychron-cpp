# Per-vendor byte codecs: pure functions, zero I/O. Each vendor adds
# include/pychron/codecs/<vendor>.hpp and src/<vendor>.cpp (CONVENTIONS.md).
file(GLOB PYCHRON_CODEC_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(pychron_codecs STATIC ${PYCHRON_CODEC_SOURCES})
add_library(pychron::codecs ALIAS pychron_codecs)

target_include_directories(pychron_codecs PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(pychron_codecs PUBLIC cxx_std_20)
# Codecs need Bytes/ReadSpec vocabulary only; they never open a transport.
target_link_libraries(pychron_codecs PUBLIC pychron::transport)

pychron_set_warnings(pychron_codecs)
