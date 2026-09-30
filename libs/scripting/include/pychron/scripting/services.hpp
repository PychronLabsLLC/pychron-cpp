#pragma once

// Non-device services a script run may reach, and gosub resolution.
//
//   acquire wait release set_resource get_resource_value   IResourceService
//   get_intensity (post_measurement only)                  IIntensitySource
//   hook `api` object (measurement hooks)                  IMeasurementApi
//   gosub                                                  IScriptResolver

#include <filesystem>
#include <map>
#include <string>
#include <string_view>

#include "pychron/core/error.hpp"
#include "pychron/scripting/script.hpp"

namespace pychron::scripting {

// Shared named resources (mutex-like flags with a value) coordinating
// scripts across systems.
struct IResourceService {
  virtual ~IResourceService() = default;
  // true if taken; false if another holder has it.
  virtual Result<bool> try_acquire(std::string_view name) = 0;
  virtual Result<void> release(std::string_view name) = 0;
  virtual Result<void> set_value(std::string_view name, double value) = 0;
  virtual Result<double> get_value(std::string_view name) = 0;
};

// Latest intensity of an isotope or detector key (e.g. "Ar40" or "H1").
struct IIntensitySource {
  virtual ~IIntensitySource() = default;
  virtual Result<double> intensity(std::string_view key) = 0;
};

// Typed API handed to measurement hooks (spec 4.3). A hook cannot replace the
// block sequence; it can only use these.
struct IMeasurementApi {
  virtual ~IMeasurementApi() = default;
  // Put `isotope` on `detector`.
  virtual Result<void> position(std::string_view isotope, std::string_view detector) = 0;
  virtual Result<void> acquire(int counts, double integration_time_s) = 0;
  virtual Result<void> open(std::string_view valve) = 0;
  virtual Result<void> close(std::string_view valve) = 0;
  // A conditional in the conditionals DSL, e.g. "Ar40 > 100 -> truncate".
  virtual Result<void> add_conditional(std::string_view spec) = 0;
  virtual Result<void> truncate(bool quick) = 0;
  virtual void log(std::string_view message) = 0;
};

// Finds gosub targets. pychron names use ':' as a path separator
// ("common:wait_for_access"); ".py" is implied.
struct IScriptResolver {
  virtual ~IScriptResolver() = default;
  // Config error when not found.
  virtual Result<Script> resolve(std::string_view name, ScriptKind from) const = 0;
};

// Looks under <root>/<kind dir>/ then <root>/lib/. Names with ".." or an
// absolute path are rejected.
class DirectoryScriptResolver final : public IScriptResolver {
 public:
  explicit DirectoryScriptResolver(std::filesystem::path root) : root_(std::move(root)) {}
  Result<Script> resolve(std::string_view name, ScriptKind from) const override;

 private:
  std::filesystem::path root_;
};

// In-memory scripts by name (with or without ".py").
class MapScriptResolver final : public IScriptResolver {
 public:
  void add(std::string name, std::string text) { scripts_[std::move(name)] = std::move(text); }
  Result<Script> resolve(std::string_view name, ScriptKind from) const override;

 private:
  std::map<std::string, std::string, std::less<>> scripts_;
};

// "common:wait" -> "common/wait.py". Config error for unsafe names.
Result<std::string> normalize_script_name(std::string_view name);

}  // namespace pychron::scripting
