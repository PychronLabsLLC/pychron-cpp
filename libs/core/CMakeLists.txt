find_package(Threads REQUIRED)

add_library(pychron_core
  src/error.cpp
  src/clock.cpp
  src/virtual_clock.cpp
  src/time_series_ring.cpp
  src/signal_bus.cpp
  src/logger.cpp
  src/log_match.cpp
  src/log_hub.cpp
  src/scheduler.cpp
  src/clock_pump.cpp
  src/process.cpp
  src/sha256.cpp
  src/sha1.cpp
  src/calendar.cpp
  src/user_file.cpp
  src/config/diagnostic.cpp
  src/config/loader.cpp
  src/config/validate.cpp
)
add_library(pychron::core ALIAS pychron_core)

target_include_directories(pychron_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(pychron_core PUBLIC cxx_std_20)

# toml::table appears in public config headers (DriverConfig::options), so the
# toml++ configuration must match in every translation unit: no exceptions.
target_compile_definitions(pychron_core PUBLIC TOML_EXCEPTIONS=0)
target_link_libraries(pychron_core PUBLIC tomlplusplus::tomlplusplus Threads::Threads)
# spdlog is an implementation detail of LogHub: never exposed (spec 4.6).
target_link_libraries(pychron_core PRIVATE spdlog::spdlog)

pychron_set_warnings(pychron_core)
