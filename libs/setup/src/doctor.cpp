#include "pychron/setup/doctor.hpp"
#include "pychron/setup/connection.hpp"
#include "pychron/setup/installer.hpp"

#include <fstream>
#include <sstream>

#include <toml++/toml.hpp>

#include "pychron/core/config/loader.hpp"
#include "pychron/core/process.hpp"
#include "pychron/experiment/lab/lab.hpp"
#include "pychron/systems/canvas/loader.hpp"
#include "pychron/systems/spectrometer/data_dir.hpp"
#include "pychron/transport/tcp_transport.hpp"

namespace pychron::setup {

namespace fs = std::filesystem;

namespace {

std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

Check ok(std::string name, std::string detail = {}) { return {Check::Status::Ok, std::move(name), std::move(detail), {}}; }
Check warn(std::string name, std::string detail, std::string hint = {}) {
  return {Check::Status::Warn, std::move(name), std::move(detail), std::move(hint)};
}
Check failed(std::string name, std::string detail, std::string hint = {}) {
  return {Check::Status::Fail, std::move(name), std::move(detail), std::move(hint)};
}

std::string first_line(const std::string& s) { return s.substr(0, s.find('\n')); }

std::string reconfigure_hint(const SiteInstall& i) { return "elctl init --reconfigure --install " + i.name; }

void check_writable(std::vector<Check>& out, const std::string& name, const fs::path& dir) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  const fs::path probe = dir / ".pychron-doctor-probe";
  {
    std::ofstream f(probe);
    f << "x";
    if (!f) {
      out.push_back(failed(name, dir.string() + " is not writable", "check the folder's permissions"));
      return;
    }
  }
  fs::remove(probe, ec);
  out.push_back(ok(name, dir.string()));
}

void check_instrument(std::vector<Check>& out, const SiteInstall& install, const DoctorOptions& options) {
  const fs::path line = install.path(install.line);
  const fs::path spectrometer = install.path(install.spectrometer);
  auto system = config::load_system_config(line);
  if (system) {
    const bool simulated = std::all_of(system->transports.begin(), system->transports.end(),
                                       [](const auto& t) { return t.second.kind == config::TransportKind::Sim; });
    out.push_back(simulated && !install.simulation
                      ? warn("extraction line", line.string() + " loads, but every transport is simulated",
                             "describe your line's controllers in extraction_line.toml (see CALIBRATE.md)")
                      : ok("extraction line", line.string()));
  } else {
    out.push_back(failed("extraction line", first_line(system.error().what), "fix " + line.string()));
  }
  if (!install.canvas.empty()) {
    auto canvas = canvas::load_canvas(install.path(install.canvas));
    out.push_back(canvas ? ok("canvas", install.path(install.canvas).string())
                         : failed("canvas", first_line(canvas.error().what)));
  }
  auto spec = spectrometer::cfg::load_spectrometer(spectrometer);
  if (spec) out.push_back(ok("spectrometer", spectrometer.string()));
  else out.push_back(failed("spectrometer", first_line(to_string(spec.error())), "fix " + spectrometer.string()));

  auto lab = experiment::lab::load_lab({install.root, line, spectrometer});
  if (lab.problems.empty()) out.push_back(ok("lab files", install.root.string()));
  else out.push_back(failed("lab files", first_line(lab.problems.front()), std::to_string(lab.problems.size()) + " problem(s)"));

  // Placeholders matter once the install drives hardware.
  if (auto record = read_install_record(install.root)) {
    std::vector<std::string> marked;
    for (const auto& [rel, hash] : record->files) {
      const std::string text = read_file(install.root / rel);
      if (text.find("SIMULATION PLACEHOLDER") != std::string::npos || text.find("CONFIRM") != std::string::npos)
        marked.push_back(rel);
    }
    if (!marked.empty()) {
      std::string list;
      for (const auto& m : marked) list += (list.empty() ? "" : ", ") + m;
      out.push_back(install.simulation ? ok("placeholders", "in simulation; still to calibrate: " + list)
                                       : warn("placeholders", "still marked SIMULATION PLACEHOLDER or CONFIRM: " + list,
                                              "replace them with the instrument's values (CALIBRATE.md)"));
    }
  }

  if (options.probe && spec) {
    for (const auto& [name, t] : spec->config.transports) {
      if (t.kind != spectrometer::cfg::TransportKind::Tcp) continue;
      TransportOptions to;
      to.name = name;
      to.timeout = std::chrono::milliseconds(2000);
      TcpTransport tcp(TcpSettings{t.host, static_cast<std::uint16_t>(t.tcp_port)}, to);
      auto opened = tcp.open();
      const std::string where = t.host + ":" + std::to_string(t.tcp_port);
      out.push_back(opened ? ok("connect " + name, where)
                           : warn("connect " + name, where + ": " + opened.error().what,
                                  "is the instrument computer on and reachable from here?"));
      tcp.close();
    }
    // Then what the programs do at start-up: the drivers' connect step.
    auto connected = connect_spectrometer(spectrometer);
    out.push_back(connected ? ok("connect spectrometer", *connected)
                            : warn("connect spectrometer", first_line(connected.error().what),
                                   "check the address, port and login in spectrometer.toml / spectrometer.local.toml"));
  }

  // Email and webhook notifications run curl.
  if (lab.notifications.email.size() + lab.notifications.webhooks.size() > 0) {
    auto r = run_process(ProcessSpec{{lab.notifications.curl, "--version"}, {}, {}, std::chrono::seconds(10)});
    out.push_back(r && r->exit_code == 0 ? ok("curl", lab.notifications.curl)
                                         : warn("curl", "cannot run " + lab.notifications.curl,
                                                "install curl, or set curl = \"<path>\" in notifications.toml"));
  }
}

void check_data_reduction(std::vector<Check>& out, const SiteInstall& install, const DoctorOptions& options) {
  if (install.database.empty()) {
    out.push_back(failed("database", "the install names no database", reconfigure_hint(install)));
    return;
  }
  if (!options.open_database) {
    out.push_back(warn("database", install.database + " (not checked: built without the database library)"));
    return;
  }
  auto url = database_url(install);
  if (!url) {
    out.push_back(failed("database", url.error().what, reconfigure_hint(install)));
    return;
  }
  auto opened = options.open_database(*url);
  out.push_back(opened ? ok("database", install.database + " (" + *opened + ")")
                       : failed("database", install.database + ": " + first_line(opened.error().what),
                                install.database.starts_with("postgresql")
                                    ? "check the server, user and password; ask your administrator about the schema"
                                    : "elctl init --reconfigure recreates a missing local database"));
}

}  // namespace

std::string_view to_string(Check::Status s) noexcept {
  switch (s) {
    case Check::Status::Ok: return "OK";
    case Check::Status::Warn: return "WARN";
    case Check::Status::Fail: return "FAIL";
  }
  return "?";
}

bool any_fail(const std::vector<Check>& checks) {
  return std::any_of(checks.begin(), checks.end(), [](const Check& c) { return c.status == Check::Status::Fail; });
}
bool any_warn(const std::vector<Check>& checks) {
  return std::any_of(checks.begin(), checks.end(), [](const Check& c) { return c.status == Check::Status::Warn; });
}

std::vector<Check> doctor(const SiteInstall& install, const DoctorOptions& options) {
  std::vector<Check> out;
  std::error_code ec;
  if (!fs::is_directory(install.root, ec)) {
    out.push_back(failed("install folder", install.root.string() + " does not exist",
                         "elctl init " + install.profile + " --root " + install.root.string()));
    return out;
  }
  out.push_back(ok("install folder", install.root.string()));

  auto record = read_install_record(install.root);
  if (!record) {
    out.push_back(warn("install record", record.error().what, "this folder was not set up by elctl init"));
  } else {
    std::vector<std::string> missing, edited;
    for (const auto& [rel, hash] : record->files) {
      if (!fs::exists(install.root / rel, ec)) missing.push_back(rel);
      else if (sha256_hex(read_file(install.root / rel)) != hash) edited.push_back(rel);
    }
    if (!missing.empty()) {
      std::string list;
      for (const auto& m : missing) list += (list.empty() ? "" : ", ") + m;
      out.push_back(failed("installed files", "missing: " + list, reconfigure_hint(install) + " puts them back"));
    } else {
      out.push_back(ok("installed files", std::to_string(record->files.size()) + " present" +
                                              (edited.empty() ? "" : ", " + std::to_string(edited.size()) + " edited")));
    }
    if (options.library != nullptr) {
      for (const auto& [name, version] : record->versions) {
        const auto* p = options.library->find(name);
        if (p != nullptr && p->version > version)
          out.push_back(warn("profile " + name, "installed version " + std::to_string(version) + ", now " +
                                                    std::to_string(p->version),
                             reconfigure_hint(install) + " brings the new files (edited files get a .new beside them)"));
      }
    }
  }

  if (install.kind == "data_reduction") {
    check_data_reduction(out, install, options);
    check_writable(out, "data folder", install.path(install.data.empty() ? "data" : install.data));
  } else {
    check_instrument(out, install, options);
    check_writable(out, "data folder", install.path(install.data.empty() ? "data" : install.data));
  }
  return out;
}

SiteInstall site_install(const InstallPlan& plan, const std::string& name) {
  SiteInstall s;
  s.name = name;
  s.profile = plan.profile.top.name;
  s.root = plan.root;
  s.data = "data";
  auto text = [&](const char* id) {
    auto it = plan.answers.find(id);
    return it == plan.answers.end() ? std::string{} : to_text(it->second);
  };
  if (plan.profile.top.kind == ProfileKind::DataReduction) {
    s.kind = "data_reduction";
    s.database = database_url_for(plan.answers, plan.root, false);
  } else {
    s.kind = "instrument";
    s.line = "extraction_line.toml";
    s.canvas = "canvas.toml";
    s.spectrometer = "spectrometer.toml";
    s.simulation = text("simulation") == "true";
  }
  return s;
}

Result<std::string> database_url(const SiteInstall& install) {
  const std::string& url = install.database;
  if (!url.starts_with("postgresql://")) return url;
  const fs::path creds = install.root / ".pychron" / "credentials.toml";
  std::error_code ec;
  if (!fs::exists(creds, ec)) return url;  // a server that needs no password
  auto parsed = toml::parse(read_file(creds), creds.string());
  if (!parsed) return fail(ErrorKind::Config, creds.string() + ": " + std::string(parsed.error().description()));
  const auto password = parsed.table()["database"]["password"].value_or(std::string{});
  if (password.empty()) return url;
  const auto at = url.find('@');
  if (at == std::string::npos) return fail(ErrorKind::Config, "database URL has no user: " + url);
  return url.substr(0, at) + ":" + percent_encode(password) + url.substr(at);
}

}  // namespace pychron::setup
