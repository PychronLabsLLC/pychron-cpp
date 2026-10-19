#include "data_source.hpp"

#include <charconv>
#include <cstddef>
#include <filesystem>
#include <istream>
#include <ostream>
#include <system_error>
#include <string_view>
#include <utility>
#include <variant>

#include "pychron/setup/data_source.hpp"
#include "pychron/setup/doctor.hpp"
#include "pychron/setup/installer.hpp"
#include "pychron/setup/profile.hpp"
#include "pychron/setup/site.hpp"
#include "password_prompt.hpp"
#include "setup.hpp"

#ifdef PYCHRON_ELCTL_HAS_STORE
#include "entry.hpp"
#endif

namespace elctl {

namespace fs = std::filesystem;
using namespace pychron;
using namespace pychron::setup;

namespace {

constexpr const char* kDataSourceUsage =
    "usage: elctl [--install NAME] data-source\n"
    "       elctl [--install NAME] data-source set\n"
    "             (--local [FILE] | --server HOST[:PORT]/DB --user USER [--password-stdin])\n"
    "             [--data DIR] [--no-check] [--yes]\n";

int usage(const std::string& message, Io io) {
  io.err << "elctl data-source: " << message << "\n" << kDataSourceUsage;
  return kUsage;
}

int failed(const std::string& message, Io io) {
  io.err << "error: " << message << "\n";
  return kFailed;
}

// What `set` was given; nullopt: kept as the install has it.
struct SetArgs {
  std::optional<fs::path> local;         // --local; "data/pychron.db" when given without a file
  std::optional<ServerDatabase> server;  // --server and --user; password from --password-stdin
  std::optional<std::string> data;
  bool password_stdin = false;
  bool no_check = false;
  bool assume_yes = false;
};

// HOST[:PORT]/DB, port 5432 when not given.
std::variant<ServerDatabase, std::string> parse_server(const std::string& text) {
  const auto slash = text.find('/');
  if (slash == std::string::npos || slash + 1 == text.size())
    return std::string("--server takes HOST[:PORT]/DB; got '" + text + "'");
  ServerDatabase server;
  server.name = text.substr(slash + 1);
  const std::string hostport = text.substr(0, slash);
  const auto colon = hostport.rfind(':');
  server.host = hostport.substr(0, colon);
  if (colon != std::string::npos) {
    const std::string port = hostport.substr(colon + 1);
    int n = 0;
    const auto [end, code] = std::from_chars(port.data(), port.data() + port.size(), n);
    if (port.empty() || code != std::errc{} || end != port.data() + port.size())
      return std::string("--server port '" + port + "' is not a number");
    server.port = n;
  }
  if (server.host.empty()) return std::string("--server takes HOST[:PORT]/DB; got '" + text + "'");
  return server;
}

std::variant<SetArgs, std::string> parse_set(const std::vector<std::string>& args) {
  SetArgs set;
  std::optional<std::string> server;
  std::optional<std::string> user;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    const bool has_value = i + 1 < args.size() && !args[i + 1].starts_with('-');
    if (a == "--yes" || a == "-y") {
      set.assume_yes = true;
    } else if (a == "--local") {
      set.local = has_value ? fs::path(args[++i]) : fs::path(LocalDatabase{}.file);
    } else if (a == "--data") {
      if (!has_value) return std::string("--data needs a folder");
      set.data = args[++i];
    } else if (a == "--server") {
      if (!has_value) return std::string("--server needs HOST[:PORT]/DB");
      server = args[++i];
    } else if (a == "--user") {
      if (!has_value) return std::string("--user needs a name");
      user = args[++i];
    } else if (a == "--password-stdin") {
      set.password_stdin = true;
    } else if (a == "--no-check") {
      set.no_check = true;
    } else {
      return "unexpected '" + a + "'";
    }
  }
  if (server && set.local) return std::string("--local and --server name two databases; give one");
  if (!server && user) return std::string("--user goes with --server");
  if (!server && set.password_stdin) return std::string("--password-stdin goes with --server");
  if (server) {
    if (!user || user->empty()) return std::string("--server needs --user USER");
    auto parsed = parse_server(*server);
    if (const auto* message = std::get_if<std::string>(&parsed)) return *message;
    set.server = std::get<ServerDatabase>(std::move(parsed));
    set.server->user = *user;
  }
  if (!set.local && !set.server && !set.data)
    return std::string("nothing to set: give --local [FILE], --server HOST[:PORT]/DB or --data DIR");
  return set;
}

DataSourceOpeners openers() {
  DataSourceOpeners o;
#ifdef PYCHRON_ELCTL_HAS_STORE
  o.open_database = [](const std::string& url, bool migrate) { return open_store(url, migrate); };
  o.seed_database = [](const std::string& url, const fs::path& seed, bool migrate) {
    return seed_database(url, seed, migrate, false);
  };
#endif
  return o;
}

void print_lines(std::ostream& out, const std::string& text, std::string_view indent) {
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find('\n', start);
    out << indent << text.substr(start, end == std::string::npos ? std::string::npos : end - start) << "\n";
    if (end == std::string::npos) break;
    start = end + 1;
  }
}

int show(const SiteInstall& install, Io io) {
  auto current = current_data_source(install);
  if (!current) return failed(current.error().what, io);
  io.out << "install " << install.name << "\n" << describe(*current, install.root) << "\n";
  if (std::holds_alternative<ServerDatabase>(current->database)) {
    auto kept = read_kept_password(install.root);
    if (!kept) return failed(kept.error().what, io);
    io.out << (*kept ? "password kept" : "no password kept") << "\n";
  }
  return kOk;
}

// One line of standard input, without its line end; null when input ended.
std::optional<std::string> read_line(std::istream& in) {
  std::string line;
  if (!std::getline(in, line)) return std::nullopt;
  if (!line.empty() && line.back() == '\r') line.pop_back();
  return line;
}

bool same_server(const ServerDatabase& a, const ServerDatabase& b) {
  return a.host == b.host && a.port == b.port && a.name == b.name && a.user == b.user;
}

int set(const SiteInstall& install, SetArgs given, Io io) {
  auto current = current_data_source(install);
  if (!current) return failed(current.error().what, io);
  DataSource to = *current;
  if (given.local) to.database = LocalDatabase{*given.local};
  if (given.server) {
    // Never on the command line: standard input, a prompt without echo, or
    // the kept one.
    ServerDatabase server = *given.server;
    const auto* now = std::get_if<ServerDatabase>(&current->database);
    if (given.password_stdin) {
      server.password = read_line(io.in);
      if (!server.password) return usage("--password-stdin: standard input ended before a password", io);
    } else if ((now == nullptr || !same_server(*now, server)) && at_terminal(io)) {
      server.password = read_password(io, "Password for " + server.user + " at " + server.host + ":" +
                                              std::to_string(server.port) + "/" + server.name + ": ");
      if (!server.password) return failed("no password read", io);
    }
    to.database = std::move(server);
  }
  if (given.data) to.data = *given.data;

  auto change = plan_data_source(install, to);
  if (!change) return failed(change.error().what, io);
  if (to == *current) {
    io.out << "nothing to change\n";
    return kOk;
  }
  io.out << "install " << install.name << "\n  before:\n";
  print_lines(io.out, describe(*current, install.root), "    ");
  io.out << "  after:\n";
  print_lines(io.out, describe(to, install.root), "    ");
  for (const auto& note : change->notes) io.out << "  " << note << "\n";
  if (!given.assume_yes && !yes(io.in, io.out, "Write them?")) {
    io.out << "nothing written\n";
    return kFailed;
  }

  const DataSourceOpeners open = openers();
  DataSourceCheck check;  // with --no-check: nothing opened, so nothing seeded
  if (given.no_check) {
    io.out << "skip  check: --no-check\n";
  } else {
    auto checked = check_data_source(*change, to, open, true);
    if (!checked) return failed(checked.error().what + "\nnothing written", io);
    check = std::move(*checked);
    for (const auto& line : check.said) io.out << line << "\n";
  }

  const fs::path site_path = default_site_path();
  if (auto applied = apply_data_source(*change, to, site_path); !applied) return failed(applied.error().what, io);
  io.out << "written: install '" << install.name << "' in " << site_path.string() << "\n";
  for (const auto& line : seed_data_source(*change, to, open, check)) io.out << line << "\n";

  // What the install now looks like to doctor; its findings are said, the
  // change is written whatever they are.
  const Resources where = find_resources();
  auto library = ProfileLibrary::load(where.profiles, where.examples);
  io.out << "\n";
  (void)report_doctor(change->install, library ? &*library : nullptr, false, false, io);
  return kOk;
}

}  // namespace

// cppcheck-suppress passedByValue ; Io is three references, and every command takes it by value
int data_source_command(const std::vector<std::string>& args, const std::optional<std::string>& install, Io io) {
  const bool setting = !args.empty() && args.front() == "set";
  if (!args.empty() && !setting) return usage("unexpected '" + args.front() + "'", io);
  std::optional<SetArgs> given;
  if (setting) {
    auto parsed = parse_set(std::vector<std::string>(args.begin() + 1, args.end()));
    if (const auto* message = std::get_if<std::string>(&parsed)) return usage(*message, io);
    given = std::get<SetArgs>(std::move(parsed));
  }
  auto resolved = resolve_install(install);
  if (!resolved) return failed(resolved.error().what, io);
  return given ? set(*resolved, std::move(*given), io) : show(*resolved, io);
}

}  // namespace elctl
