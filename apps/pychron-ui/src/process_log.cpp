#include "process_log.hpp"

#include <cstdio>
#include <string>
#include <utility>

#include "pychron/core/config/logging_file.hpp"
#include "pychron/core/logger.hpp"
#include "pychron/setup/installer.hpp"

namespace pychron::ui {

std::shared_ptr<LogHub> open_process_log(const std::optional<setup::SiteInstall>& install,
                                         const std::optional<std::filesystem::path>& line_config,
                                         const Clock& clock, bool simulated, HostOs os, const EnvLookup& env,
                                         std::string_view clock_name) {
  std::optional<std::filesystem::path> root;
  if (install) root = install->root;

  auto sources = config::load_logging_config(root, line_config);
  if (!sources) {
    std::fprintf(stderr, "pychron-ui: logging disabled: %s\n", sources.error().what.c_str());
    return nullptr;
  }
  config::LoggingConfig cfg = std::move(sources->config);
  cfg.file_name = "pychron.log";
  resolve_log_dir(cfg, root, os, env);

  auto created = LogHub::create(cfg, clock);
  if (!created) {
    std::fprintf(stderr, "pychron-ui: logging disabled: %s\n", created.error().what.c_str());
    return nullptr;
  }
  std::shared_ptr<LogHub> hub = std::move(*created);

  const auto notes = hub->logger("pychron");
  for (const auto& [level, text] : sources->notes) notes.log(level, text);

  SessionInfo info;
  info.app = "pychron-ui";
  info.version = std::string(setup::version());
  info.install_root = std::move(root);
  if (install) info.profiles = install->profile;
  info.simulated = simulated;
  info.clock = std::string(clock_name);
  hub->begin_session(info);
  return hub;
}

}  // namespace pychron::ui
