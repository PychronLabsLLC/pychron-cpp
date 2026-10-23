#include "process_log.hpp"

#include <cstdio>
#include <string>
#include <utility>

#include "pychron/core/config/logging_file.hpp"
#include "pychron/core/logger.hpp"
#include "pychron/setup/installer.hpp"

namespace pychron::ui {

ProcessLog open_process_log(const std::optional<setup::SiteInstall>& install,
                            const std::optional<std::filesystem::path>& line_config, const Clock& clock,
                            bool simulated, HostOs os, const EnvLookup& env, std::string_view clock_name) {
  std::optional<std::filesystem::path> root;
  if (install) root = install->root;

  ProcessLog out;
  // A config that does not load leaves the built-in defaults (and the default
  // folder) in force: the log is where it is looked for, and says why.
  auto sources = config::load_logging_config(root, line_config);
  if (!sources) {
    std::string why = sources.error().what + "; logging runs on the built-in defaults";
    std::fprintf(stderr, "pychron-ui: %s\n", why.c_str());
    out.notices.emplace_back(LogLevel::Error, std::move(why));
    sources = config::LoggingSources{};
  }
  config::LoggingConfig cfg = std::move(sources->config);
  cfg.file_name = "pychron.log";
  const bool dir_unset = !cfg.dir.has_value();
  resolve_log_dir(cfg, root, os, env);
  if (dir_unset && cfg.dir->empty()) {
    std::string why = std::string("no log folder: ") + log_dir_env_name(os) + " is not set";
    if (!cfg.echo_stderr) std::fprintf(stderr, "pychron-ui: %s\n", why.c_str());
    out.notices.emplace_back(LogLevel::Warn, std::move(why));
  }

  auto created = LogHub::create(cfg, clock);
  if (!created) {
    std::fprintf(stderr, "pychron-ui: logging disabled: %s\n", created.error().what.c_str());
    return out;
  }
  std::shared_ptr<LogHub> hub = std::move(*created);

  const auto notes = hub->logger("logging");
  for (const auto& [level, text] : out.notices) notes.log(level, text);
  for (const auto& [level, text] : sources->notes) notes.log(level, text);

  SessionInfo info;
  info.app = "pychron-ui";
  info.version = std::string(setup::version());
  info.install_root = std::move(root);
  if (install) info.profiles = install->profile;
  info.simulated = simulated;
  info.clock = std::string(clock_name);
  hub->begin_session(info);
  out.hub = std::move(hub);
  return out;
}

}  // namespace pychron::ui
