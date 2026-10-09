#include "db.hpp"

#include <filesystem>
#include <ostream>
#include <string_view>
#include <system_error>

#include "pychron/persistence/store.hpp"

namespace elctl {

namespace {

namespace fs = std::filesystem;
namespace ps = pychron::persistence;

constexpr const char* kShortUsage =
    "usage: elctl db status --db <url>     is the store's schema up to date? (exit 1 when not)\n"
    "       elctl db migrate --db <url>    apply the migrations the store lacks\n"
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

}  // namespace

int db_command(const std::vector<std::string>& args, Io io) {
  if (args.empty() || args[0] == "help" || args[0] == "--help") {
    io.out << kShortUsage;
    return args.empty() ? kUsage : kOk;
  }
  const std::string& what = args[0];
  if (what != "status" && what != "migrate") return usage(io, "unknown action '" + what + "'");
  std::string url;
  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--db" && i + 1 < args.size()) {
      url = args[++i];
    } else {
      return usage(io, "unexpected '" + args[i] + "'");
    }
  }
  if (url.empty()) return usage(io, "--db <url> is required");
  if (const std::string why = not_a_store(url); !why.empty()) return fatal(io, why);

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
