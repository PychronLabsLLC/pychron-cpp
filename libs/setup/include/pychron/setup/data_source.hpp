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
#include <functional>
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

// Writes a planned change, in this order, and nothing else:
//  1. <root>/.pychron/credentials.toml, owner-only, as the store-common
//     profile renders it: only for a server with a password given. A kept
//     password (nullopt) and a local source leave the file alone.
//  2. <root>/.pychron/install.toml, when the install has one: answers
//     data_source, db_host, db_port, db_name, db_user, db_file and
//     data_folder replaced (server keys erased for a local source, db_file
//     for a server one), and the credentials file's sha256 when step 1 wrote
//     it, so a later reconfigure sees it as its own. Never the password.
//  3. The install's entry in the site file at `site_path`: database and data
//     replaced, every other field and entry as it was (an entry not there is
//     added).
// The site file is read before anything is written: one that does not load
// refuses the change with nothing written.
Result<void> apply_data_source(const DataSourceChange& change, const DataSource& to,
                               const std::filesystem::path& site_path);

// What a change opens the store with, injected by an app built with the store
// (as `DoctorOptions::open_database`): libs/setup does not link it.
struct DataSourceOpeners {
  // Opens url (with password); migrate: made and migrated. Result describes it
  // ("schema version 12"). Empty: database not checked (built without the store).
  std::function<Result<std::string>(const std::string& url, bool migrate)> open_database;
  // As SetupWizard::SeedDatabase. Empty: not seeded.
  std::function<Result<std::string>(const std::string& url, const std::filesystem::path& seed_file, bool migrate)>
      seed_database;
};

// What check_data_source found. A default one (nothing opened) stands for a
// change written without checking (`--no-check`, "Save without checking").
struct DataSourceCheck {
  std::vector<std::string> said;  // lines said on success
  bool made_file = false;         // a missing local file was made and migrated (create = true)
  bool opened = false;            // the database (made, existing or server) opened
};

// Checks before a change is written (spec 3.3). Data folder: with `create`
// made when missing, then probed with a file made and removed; refused when
// not writable. Database: a missing local file is made (folder too) and
// migrated with `create`; an existing one is opened with migrate = false
// (an empty file, or one whose open fails, is refused); a server is opened
// with migrate = false and the password given or kept. `create` false (Test
// connection) writes nothing: a missing folder or file is said as "will be
// made". Refused with ErrorKind::Config, reason in plain words, never the
// password.
Result<DataSourceCheck> check_data_source(const DataSourceChange& change, const DataSource& to,
                                          const DataSourceOpeners& openers, bool create);

// After apply (spec 3.3): seeded (ensure-only, <root>/seed.toml) only when the
// install is an instrument one AND `to` is either a local file that this change
// made (missing before check_data_source with create = true) or a server that
// opened. An existing local file is never seeded: an existing store is not
// touched. Lines say what was done or why skipped, never an error.
// `check`: what check_data_source returned; a default one (`--no-check`, or
// "Save without checking") opened nothing, so nothing is seeded and that is said.
std::vector<std::string> seed_data_source(const DataSourceChange& change, const DataSource& to,
                                          const DataSourceOpeners& openers, const DataSourceCheck& check);

}  // namespace pychron::setup
