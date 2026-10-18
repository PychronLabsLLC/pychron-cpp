#include "data_source.hpp"

#include <cstddef>
#include <filesystem>
#include <ostream>
#include <string_view>
#include <utility>
#include <variant>

#include "pychron/setup/data_source.hpp"
#include "pychron/setup/doctor.hpp"
#include "pychron/setup/installer.hpp"
#include "pychron/setup/profile.hpp"
#include "pychron/setup/site.hpp"
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
    "       elctl [--install NAME] data-source set [--local [FILE]] [--data DIR] [--yes]\n";

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
  std::optional<fs::path> local;  // --local; "data/pychron.db" when given without a file
  std::optional<std::string> data;
  bool assume_yes = false;
};

std::variant<SetArgs, std::string> parse_set(const std::vector<std::string>& args) {
  SetArgs set;
  bool server = false;
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
      server = true;
      if (has_value) ++i;
    } else {
      return "unexpected '" + a + "'";
    }
  }
  if (server && set.local) return std::string("--local and --server name two databases; give one");
  if (server) return std::string("--server is not supported yet");
  if (!set.local && !set.data) return std::string("nothing to set: give --local [FILE] or --data DIR");
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

int set(const SiteInstall& install, const SetArgs& given, Io io) {
  auto current = current_data_source(install);
  if (!current) return failed(current.error().what, io);
  DataSource to = *current;
  if (given.local) to.database = LocalDatabase{*given.local};
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
  auto check = check_data_source(*change, to, open, true);
  if (!check) return failed(check.error().what + "\nnothing written", io);
  for (const auto& line : check->said) io.out << line << "\n";

  const fs::path site_path = default_site_path();
  if (auto applied = apply_data_source(*change, to, site_path); !applied) return failed(applied.error().what, io);
  io.out << "written: install '" << install.name << "' in " << site_path.string() << "\n";
  for (const auto& line : seed_data_source(*change, to, open, *check)) io.out << line << "\n";

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
  return given ? set(*resolved, *given, io) : show(*resolved, io);
}

}  // namespace elctl
