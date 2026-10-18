#pragma once

// Changing an install's data source after install (data source editing spec,
// docs/superpowers/specs/2026-10-18-data-source-editing-design.md, 3.1): the
// database it reads and writes (a local SQLite file or the lab's PostgreSQL
// server) and its data folder. Qt-free and persistence-free, as the rest of
// libs/setup: elctl and pychron-ui share it.
//
// A change is planned first (every refusal happens there, nothing written),
// then applied.

#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/setup/site.hpp"

namespace pychron::setup {

struct LocalDatabase {
  std::filesystem::path file = "data/pychron.db";  // relative to root, or absolute

  friend bool operator==(const LocalDatabase&, const LocalDatabase&) = default;
};

struct ServerDatabase {
  std::string host;
  int port = 5432;
  std::string name, user;
  std::optional<std::string> password;  // nullopt: keep the one the install has

  friend bool operator==(const ServerDatabase&, const ServerDatabase&) = default;
};

struct DataSource {
  std::variant<LocalDatabase, ServerDatabase> database;
  std::string data = "data";  // data folder, relative to root, or absolute

  friend bool operator==(const DataSource&, const DataSource&) = default;
};

// What the install uses now: from its install record's answers, its site entry
// as fallback (an install without a record). A record made before `db_file`
// and `data_folder` gives "data/pychron.db" and "data". Never carries a
// password (`ServerDatabase::password` is nullopt).
Result<DataSource> current_data_source(const SiteInstall& install);

// Store URL the source stands for (no password), as `SiteInstall::database`.
std::string database_url(const DataSource& source, const std::filesystem::path& root);

// The URL that opens the store: for a server, the password of `source`, else
// the one kept in <root>/.pychron/credentials.toml, percent-encoded as
// `database_url_for(..., true)`. A local source: as `database_url`.
Result<std::string> database_url_with_password(const DataSource& source, const std::filesystem::path& root);

// Two lines, "database <url>" and "data folder <path as given> (<resolved>)",
// for the before -> after both apps print.
std::string describe(const DataSource& source, const std::filesystem::path& root);

struct DataSourceChange {
  SiteInstall install;             // after: database, data set from the source
  std::vector<std::string> notes;  // said to the user (records not moved, ...)
};

// Checks `to` against `install` and says what would change. Refused with
// ErrorKind::Config (nothing written, nothing made): empty host, name or user;
// port outside 1 to 65535; empty file or data path; a file path naming an
// existing directory; an install whose root is not on disk. Writes nothing.
Result<DataSourceChange> plan_data_source(const SiteInstall& install, const DataSource& to);

}  // namespace pychron::setup
