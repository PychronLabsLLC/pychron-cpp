# Per-vendor byte codecs: pure functions, zero I/O. Header-only until the
# first vendor codec lands; vendors add sources under src/<vendor>/.
add_library(pychron_codecs INTERFACE)
add_library(pychron::codecs ALIAS pychron_codecs)

target_include_directories(pychron_codecs INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(pychron_codecs INTERFACE cxx_std_20)
# Codecs need Bytes/ReadSpec vocabulary only; they never open a transport.
target_link_libraries(pychron_codecs INTERFACE pychron::transport)
