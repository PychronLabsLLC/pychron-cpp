#include "db.hpp"

#include <charconv>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string_view>
#include <system_error>

#include "pychron/persistence/store.hpp"
#include "pychron/processing/derive.hpp"
#include "pychron/processing/store_source.hpp"

namespace elctl {

namespace {

namespace fs = std::filesystem;
namespace ps = pychron::persistence;

constexpr const char* kShortUsage =
    "usage: elctl db status --db <url>     is the store's schema up to date? (exit 1 when not)\n"
    "       elctl db migrate --db <url>    apply the migrations the store lacks\n"
    "       elctl db derive --db <url> [--all] [--limit N] [--quiet]\n"
    "                                      fill the cache of ages the browser filters by\n"
    "  <url>: sqlite:/path/to/store.db or postgresql://user:password@host/dbname\n";

int usage(Io io, const std::string& message) {
  io.err << "elctl db: " << message << '\n' << kShortUsage;
  return kUsage;
}

int fatal(Io io, const std::string& message) {
  io.err << "elctl db: " << message << '\n';
  return kUsage;
}

// A mistyped SQLite path is an error, not a new empty store.
std::string not_a_store(const std::string& url) {
  constexpr std::string_view kSqlite = "sqlite:";
  if (!url.starts_with(kSqlite) || url == "sqlite::memory:") return {};
  std::error_code code;
  const fs::path file(url.substr(kSqlite.size()));
  if (!fs::is_regular_file(file, code)) return "no database at " + file.string();
  if (fs::file_size(file, code) == 0 && !code) return file.string() + " is empty: not a pychron store";
  return {};
}

void print_schema(std::ostream& out, const std::vector<ps::AppliedMigration>& applied) {
  for (const auto& m : applied) {
    out << "  " << (m.version < 1000 ? std::string(4 - std::to_string(m.version).size(), '0') : std::string())
        << m.version << "  " << m.description << '\n';
  }
}

namespace pp = pychron::processing;

std::string counts(const pp::DeriveStats& s) {
  return std::to_string(s.seen) + " seen, " + std::to_string(s.written) + " written (" + std::to_string(s.no_age) +
         " without an age), " + std::to_string(s.current) + " current, " + std::to_string(s.failed) + " failed";
}

// elctl db derive: the backfill of the cache of ages (age cache spec 4.5).
int derive(const std::string& url, bool all, std::optional<int> limit, bool quiet, Io io) {
  // As it is: a store behind its migrations is refused, never migrated here.
  auto source = pp::StoreSource::open(pychron::persistence::StoreConfig{url, false});
  if (!source) return fatal(io, source.error().what);
  pp::DeriveOptions options;
  options.all = all;
  options.limit = limit;
  auto stats = pp::derive_ages(**source, options, [&](const pp::DeriveStats& so_far) {
    if (!quiet) io.out << "  " << counts(so_far) << '\n' << std::flush;
    return true;
  });
  if (!stats) return fatal(io, stats.error().what);
  for (const auto& f : stats->failures) io.err << "elctl db derive: " << f << '\n';
  io.out << "derived: " << counts(*stats) << '\n';
  return stats->failed > 0 ? kFailed : kOk;
}

}  // namespace

int db_command(const std::vector<std::string>& args, Io io) {
  if (args.empty() || args[0] == "help" || args[0] == "--help") {
    io.out << kShortUsage;
    return args.empty() ? kUsage : kOk;
  }
  const std::string& what = args[0];
  if (what != "status" && what != "migrate" && what != "derive") return usage(io, "unknown action '" + what + "'");
  std::string url;
  bool all = false, quiet = false;
  std::optional<int> limit;
  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--db" && i + 1 < args.size()) {
      url = args[++i];
    } else if (what == "derive" && args[i] == "--all") {
      all = true;
    } else if (what == "derive" && args[i] == "--quiet") {
      quiet = true;
    } else if (what == "derive" && args[i] == "--limit" && i + 1 < args.size()) {
      const std::string& text = args[++i];
      int n = 0;
      const auto [end, code] = std::from_chars(text.data(), text.data() + text.size(), n);
      if (code != std::errc{} || end != text.data() + text.size() || n < 1)
        return usage(io, "--limit takes a whole number, 1 or more; got '" + text + "'");
      limit = n;
    } else {
      return usage(io, "unexpected '" + args[i] + "'");
    }
  }
  if (url.empty()) return usage(io, "--db <url> is required");
  if (const std::string why = not_a_store(url); !why.empty()) return fatal(io, why);
  if (what == "derive") return derive(url, all, limit, quiet, io);

  // As it is: this succeeds only when nothing is pending.
  auto current = ps::open_store(ps::StoreConfig{url, false});
  if (current) {
    auto applied = (*current)->schema_status();
    if (!applied) return fatal(io, applied.error().what);
    io.out << "up to date: " << applied->size() << " migrations\n";
    print_schema(io.out, *applied);
    return kOk;
  }
  if (what == "status") {
    io.out << "behind: " << current.error().what << '\n';
    return kFailed;
  }

  auto migrated = ps::open_store(ps::StoreConfig{url, true});
  if (!migrated) return fatal(io, migrated.error().what);
  auto applied = (*migrated)->schema_status();
  if (!applied) return fatal(io, applied.error().what);
  io.out << "migrated: the store is up to date, " << applied->size() << " migrations\n";
  print_schema(io.out, *applied);
  return kOk;
}

}  // namespace elctl
