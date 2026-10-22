#include "process_log.hpp"

#include <iostream>
#include <optional>

#include "pychron/core/config/logging_file.hpp"
#include "pychron/core/logger.hpp"
#include "pychron/setup/installer.hpp"
#include "setup.hpp"

namespace elctl {

namespace {

// Sends std::cerr to `err` while it lives: the hub reports a folder it cannot
// make on std::cerr, and a caller that gave another stream wants it there.
// Start-up is single-threaded, so nothing else writes to std::cerr meanwhile.
class CerrRedirect {
 public:
  explicit CerrRedirect(std::ostream& err) : previous_(std::cerr.rdbuf(err.rdbuf())) {}
  ~CerrRedirect() { std::cerr.rdbuf(previous_); }
  CerrRedirect(const CerrRedirect&) = delete;
  CerrRedirect& operator=(const CerrRedirect&) = delete;

 private:
  std::streambuf* previous_;
};

}  // namespace

std::shared_ptr<pychron::LogHub> open_process_log(const std::vector<std::string>& args,
                                                  const pychron::Clock& clock, pychron::HostOs os,
                                                  const pychron::EnvLookup& env, std::ostream& err) {
  // The global options, as run() reads them; whatever it would refuse is its to report.
  std::optional<std::string> install_name;
  std::optional<std::filesystem::path> config;
  bool sim = false;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "-c" || a == "--config") {
      if (i + 1 >= args.size()) break;
      config = args[++i];
    } else if (a == "--install") {
      if (i + 1 >= args.size()) break;
      install_name = args[++i];
    } else if (a == "--sim") {
      sim = true;
    } else {
      break;
    }
  }

  // No install is fine: the OS folder.
  std::optional<pychron::setup::SiteInstall> install;
  if (install_name || !config) {
    if (auto found = resolve_install(install_name)) install = std::move(*found);
  }
  std::optional<std::filesystem::path> root;
  std::optional<std::filesystem::path> line_config = config;
  if (install) {
    sim = sim || install->simulation;
    if (!line_config && !install->line.empty()) line_config = install->path(install->line);
    root = std::move(install->root);
  }

  auto sources = pychron::config::load_logging_config(root, line_config);
  if (!sources) {
    err << "elctl: logging disabled: " << sources.error().what << '\n';
    return nullptr;
  }
  pychron::config::LoggingConfig cfg = std::move(sources->config);
  cfg.file_name = "elctl.log";
  pychron::resolve_log_dir(cfg, root, os, env);

  std::shared_ptr<pychron::LogHub> hub;
  {
    const CerrRedirect redirect(err);
    auto created = pychron::LogHub::create(cfg, clock);
    if (!created) {
      err << "elctl: logging disabled: " << created.error().what << '\n';
      return nullptr;
    }
    hub = std::move(*created);
  }

  const auto notes = hub->logger("pychron");
  for (const auto& [level, text] : sources->notes) notes.log(level, text);

  pychron::SessionInfo info;
  info.app = "elctl";
  info.version = std::string(pychron::setup::version());
  info.install_root = std::move(root);
  if (install) info.profiles = install->profile;
  info.simulated = sim;
  info.clock = "steady";
  hub->begin_session(info);
  return hub;
}

}  // namespace elctl
