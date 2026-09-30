#pragma once

// Maps a config `kind` to a driver factory plus the keys that driver reads
// from its [drivers.<name>] table. The declarations are the documentation:
// `elctl list-drivers` prints them via describe().

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <toml++/toml.hpp>

#include "pychron/core/clock.hpp"
#include "pychron/core/config/system_config.hpp"
#include "pychron/core/error.hpp"
#include "pychron/devices/device.hpp"
#include "pychron/transport/transport.hpp"

namespace pychron {

enum class KeyType { String, Integer, Float, Boolean, IntegerArray, StringArray };

std::string_view to_string(KeyType type) noexcept;

// One driver-specific key. `kind` and `transport` are common to every driver
// and never declared.
struct ConfigKey {
  std::string name;
  KeyType type = KeyType::String;
  bool required = false;
  std::string description;
};

struct DriverSchema {
  std::string kind;  // filled in by the registry
  std::string summary;
  std::vector<ConfigKey> keys;
};

// Everything a factory gets. `options` has already been checked against the
// schema: declared keys have the declared types and required keys exist.
struct DriverArgs {
  std::string name;
  Transport& transport;
  const toml::table& options;
  const Clock* clock = nullptr;
};

struct DriverContext {
  std::string name;              // device name; the kind if empty
  const Clock* clock = nullptr;  // passed to the driver for health stamps
};

using DriverFactory = std::function<Result<std::unique_ptr<Device>>(const DriverArgs&)>;

// Registration happens during static initialization (REGISTER_DRIVER) or
// single-threaded setup; lookups afterwards are read-only and thread-safe.
class DriverRegistry {
 public:
  // The process-wide registry REGISTER_DRIVER writes to.
  static DriverRegistry& global();

  // Config error, also recorded in conflicts(), if `kind` is taken or empty.
  Result<void> add(std::string kind, DriverSchema schema, DriverFactory factory);

  // Registers `D`, which provides:
  //   static DriverSchema schema();
  //   static Result<std::unique_ptr<D>> create(const DriverArgs&);
  template <class D>
  Result<void> add(std::string kind) {
    return add(std::move(kind), D::schema(), [](const DriverArgs& args) -> Result<std::unique_ptr<Device>> {
      auto made = D::create(args);
      if (!made) return fail(std::move(made).error());
      return std::unique_ptr<Device>(std::move(*made));
    });
  }

  bool contains(std::string_view kind) const;
  std::vector<std::string> kinds() const;  // sorted
  const DriverSchema* schema(std::string_view kind) const;
  std::vector<DriverSchema> schemas() const;  // sorted by kind

  // Registration failures (duplicate kinds) seen so far, for startup checks.
  const std::vector<Error>& conflicts() const noexcept { return conflicts_; }

  // Checks `options` against the kind's declared keys: unknown kind, missing
  // required keys, wrong types and undeclared keys are Config errors. All
  // problems are reported together, each prefixed `file:line:` when known.
  Result<void> validate(std::string_view kind, const toml::table& options) const;

  // validate() then construct. Every error carries the device name.
  Result<std::unique_ptr<Device>> create(std::string_view kind, Transport& transport,
                                         const toml::table& options, DriverContext context = {}) const;
  Result<std::unique_ptr<Device>> create(const config::DriverConfig& config, Transport& transport,
                                         const Clock* clock = nullptr) const;

 private:
  struct Entry {
    DriverSchema schema;
    DriverFactory factory;
  };
  std::map<std::string, Entry, std::less<>> entries_;
  std::vector<Error> conflicts_;
};

// Human-readable schema for `elctl list-drivers`:
//   <kind> - <summary>
//     <key>  <type>  required|optional  <description>
std::string describe(const DriverSchema& schema);

namespace detail {
template <class D>
bool register_driver(const char* kind) {
  return DriverRegistry::global().add<D>(kind).has_value();
}
}  // namespace detail

}  // namespace pychron

#define PYCHRON_DRIVER_CONCAT_(a, b) a##b
#define PYCHRON_DRIVER_CONCAT(a, b) PYCHRON_DRIVER_CONCAT_(a, b)

// At namespace scope in the driver's .cpp:
//   REGISTER_DRIVER("pfeiffer_maxigauge", PfeifferMaxiGauge);
// The library holding the .cpp must be linked with pychron_link_drivers()
// (see libs/devices/CMakeLists.txt) or the linker may drop it.
#define REGISTER_DRIVER(kind, Type)                                                         \
  [[maybe_unused]] static const bool PYCHRON_DRIVER_CONCAT(pychron_driver_registered_, __LINE__) = \
      ::pychron::detail::register_driver<Type>(kind)
