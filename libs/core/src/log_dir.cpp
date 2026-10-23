#include "pychron/core/log_dir.hpp"

#include "pychron/core/env.hpp"

namespace pychron {

namespace fs = std::filesystem;

HostOs host_os() noexcept {
#if defined(_WIN32)
  return HostOs::Windows;
#elif defined(__APPLE__)
  return HostOs::Mac;
#else
  return HostOs::Linux;
#endif
}

EnvLookup process_env() {
  return [](std::string_view name) { return env_var(std::string(name).c_str()); };
}

namespace {

// A variable that is set and not empty.
std::optional<fs::path> non_empty(const EnvLookup& env, std::string_view name) {
  auto value = env(name);
  if (!value || value->empty()) return std::nullopt;
  return fs::path(*value);
}

}  // namespace

fs::path default_log_dir(const std::optional<fs::path>& install_root, HostOs os, const EnvLookup& env) {
  if (install_root) return *install_root / "logs";
  switch (os) {
    case HostOs::Mac:
      if (const auto home = non_empty(env, "HOME")) return *home / "Library" / "Logs" / "pychron";
      break;
    case HostOs::Linux:
      if (const auto state = non_empty(env, "XDG_STATE_HOME")) return *state / "pychron" / "logs";
      if (const auto home = non_empty(env, "HOME")) return *home / ".local" / "state" / "pychron" / "logs";
      break;
    case HostOs::Windows:
      if (const auto local = non_empty(env, "LOCALAPPDATA")) return *local / "pychron" / "logs";
      break;
  }
  return {};
}

fs::path default_examples_state_dir(HostOs os, const EnvLookup& env) {
  switch (os) {
    case HostOs::Mac:
      // Beside the site file (setup's site.toml).
      if (const auto home = non_empty(env, "HOME")) return *home / "Library" / "Application Support" / "Pychron" / "examples";
      break;
    case HostOs::Linux:
      if (const auto state = non_empty(env, "XDG_STATE_HOME")) return *state / "pychron" / "examples";
      if (const auto home = non_empty(env, "HOME")) return *home / ".local" / "state" / "pychron" / "examples";
      break;
    case HostOs::Windows:
      if (const auto local = non_empty(env, "LOCALAPPDATA")) return *local / "pychron" / "examples";
      break;
  }
  return {};
}

const char* log_dir_env_name(HostOs os) noexcept { return os == HostOs::Windows ? "LOCALAPPDATA" : "HOME"; }

void resolve_log_dir(config::LoggingConfig& config, const std::optional<fs::path>& install_root, HostOs os,
                     const EnvLookup& env) {
  if (!config.dir) config.dir = default_log_dir(install_root, os, env);
}

}  // namespace pychron
