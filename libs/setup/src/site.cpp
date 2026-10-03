#include "pychron/setup/site.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include <toml++/toml.hpp>

#include "pychron/core/env.hpp"

namespace pychron::setup {

namespace fs = std::filesystem;

fs::path SiteInstall::path(const std::string& relative) const {
  if (relative.empty()) return {};
  const fs::path p(relative);
  return p.is_absolute() ? p : root / p;
}

const SiteInstall* SiteConfig::find(const std::string& name) const {
  auto it = std::find_if(installs.begin(), installs.end(), [&](const SiteInstall& i) { return i.name == name; });
  return it == installs.end() ? nullptr : &*it;
}

const SiteInstall* SiteConfig::pick(const std::optional<std::string>& name) const {
  if (name) return find(*name);
  if (!default_install.empty()) {
    if (const auto* d = find(default_install)) return d;
  }
  return installs.size() == 1 ? &installs.front() : nullptr;
}

void SiteConfig::upsert(SiteInstall install) {
  auto it = std::find_if(installs.begin(), installs.end(), [&](const SiteInstall& i) { return i.name == install.name; });
  if (it != installs.end()) *it = std::move(install);
  else installs.push_back(std::move(install));
}

bool SiteConfig::remove(const std::string& name) {
  const auto before = installs.size();
  std::erase_if(installs, [&](const SiteInstall& i) { return i.name == name; });
  if (default_install == name) default_install.clear();
  return installs.size() != before;
}

fs::path default_site_path() {
  if (auto p = env_var("PYCHRON_SITE_CONFIG"); p && !p->empty()) return *p;
#if defined(_WIN32)
  if (auto appdata = env_var("APPDATA")) return fs::path(*appdata) / "Pychron" / "site.toml";
  return fs::path("Pychron") / "site.toml";
#elif defined(__APPLE__)
  const fs::path home = env_var("HOME").value_or(".");
  return home / "Library" / "Application Support" / "Pychron" / "site.toml";
#else
  if (auto xdg = env_var("XDG_CONFIG_HOME"); xdg && !xdg->empty()) return fs::path(*xdg) / "pychron" / "site.toml";
  const fs::path home = env_var("HOME").value_or(".");
  return home / ".config" / "pychron" / "site.toml";
#endif
}

Result<SiteConfig> load_site(const fs::path& path) {
  std::error_code ec;
  if (!fs::exists(path, ec)) return SiteConfig{};
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  auto parsed = toml::parse(ss.str(), path.string());
  if (!parsed) return fail(ErrorKind::Config, path.string() + ": " + std::string(parsed.error().description()));
  const auto& t = parsed.table();
  SiteConfig site;
  site.default_install = t["default"].value_or(std::string{});
  if (const auto* arr = t["installs"].as_array()) {
    for (const auto& e : *arr) {
      const auto* i = e.as_table();
      if (i == nullptr) return fail(ErrorKind::Config, path.string() + ": installs must be [[installs]] tables");
      SiteInstall s;
      s.name = (*i)["name"].value_or(std::string{});
      s.kind = (*i)["kind"].value_or(std::string{});
      s.profile = (*i)["profile"].value_or(std::string{});
      s.root = (*i)["root"].value_or(std::string{});
      s.line = (*i)["line"].value_or(std::string{});
      s.canvas = (*i)["canvas"].value_or(std::string{});
      s.spectrometer = (*i)["spectrometer"].value_or(std::string{});
      s.data = (*i)["data"].value_or(std::string{});
      s.database = (*i)["database"].value_or(std::string{});
      s.simulation = (*i)["simulation"].value_or(false);
      if (s.name.empty() || s.root.empty())
        return fail(ErrorKind::Config, path.string() + ": an install needs a name and a root");
      site.installs.push_back(std::move(s));
    }
  }
  return site;
}

Result<void> save_site(const SiteConfig& site, const fs::path& path) {
  toml::table t;
  if (!site.default_install.empty()) t.insert("default", site.default_install);
  toml::array installs;
  for (const auto& s : site.installs) {
    toml::table i;
    i.insert("name", s.name);
    if (!s.kind.empty()) i.insert("kind", s.kind);
    if (!s.profile.empty()) i.insert("profile", s.profile);
    i.insert("root", s.root.string());
    for (const auto& [k, v] : std::initializer_list<std::pair<const char*, const std::string*>>{
             {"line", &s.line}, {"canvas", &s.canvas}, {"spectrometer", &s.spectrometer}, {"data", &s.data},
             {"database", &s.database}}) {
      if (!v->empty()) i.insert(k, *v);
    }
    if (s.simulation) i.insert("simulation", true);
    installs.push_back(std::move(i));
  }
  t.insert("installs", std::move(installs));
  std::error_code ec;
  if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
  if (ec) return fail(ErrorKind::Io, "cannot create " + path.parent_path().string() + ": " + ec.message());
  const fs::path tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << "# pychron installs on this computer (written by setup; safe to edit)\n" << t << "\n";
    if (!out) return fail(ErrorKind::Io, "cannot write " + tmp.string());
  }
  fs::rename(tmp, path, ec);
  if (ec) return fail(ErrorKind::Io, "cannot write " + path.string() + ": " + ec.message());
  return {};
}

}  // namespace pychron::setup
