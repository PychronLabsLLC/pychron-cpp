#pragma once

// The per-user site config (installation wizard spec section 3.2): which
// installs exist on this computer and which one the apps open by default.
//
//   default = "argus-lab"
//   [[installs]]
//   name = "argus-lab"
//   kind = "instrument"                 # instrument | data_reduction
//   profile = "argus"
//   root = "/Users/lab/Pychron/argus-lab"
//   line = "extraction_line.toml"       # relative to root
//   canvas = "canvas.toml"
//   spectrometer = "spectrometer.toml"
//   data = "data"
//   simulation = true                   # run as --sim
//   database = "sqlite:..."             # data reduction: the DVC store URL (no password)
//
// Location: $PYCHRON_SITE_CONFIG when set, else the platform's per-user
// config directory: ~/Library/Application Support/Pychron/site.toml (macOS),
// %APPDATA%\Pychron\site.toml (Windows), $XDG_CONFIG_HOME/pychron/site.toml
// or ~/.config/pychron/site.toml (elsewhere).

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "pychron/core/error.hpp"

namespace pychron::setup {

struct SiteInstall {
  std::string name, kind, profile;
  std::filesystem::path root;
  std::string line, canvas, spectrometer, data;
  std::string database;
  bool simulation = false;

  // root / relative, or empty when the relative part is.
  std::filesystem::path path(const std::string& relative) const;
};

struct SiteConfig {
  std::string default_install;
  std::vector<SiteInstall> installs;

  const SiteInstall* find(const std::string& name) const;
  // `name`, else the default, else the only install; nullptr otherwise.
  const SiteInstall* pick(const std::optional<std::string>& name = std::nullopt) const;
  // Replaces the install with the same name, or adds it.
  void upsert(SiteInstall install);
  bool remove(const std::string& name);  // the entry only, never files
};

std::filesystem::path default_site_path();
// A missing file is an empty config.
Result<SiteConfig> load_site(const std::filesystem::path& path);
Result<void> save_site(const SiteConfig& site, const std::filesystem::path& path);

}  // namespace pychron::setup
