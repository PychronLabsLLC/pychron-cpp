#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/persistence/store.hpp"
#include "tiny/db.hpp"

namespace pychron::persistence::detail {

// Generated at configure time from migrations/<dialect>/NNNN_<name>.sql.
struct EmbeddedMigration {
  const char* dialect;  // "pg" | "sqlite"
  const char* file;     // "0001_init.sql"
  const unsigned char* data;
  std::size_t size;
};
extern const EmbeddedMigration kEmbeddedMigrations[];
extern const std::size_t kEmbeddedMigrationCount;

// Splits a migration into statements: ';' outside quotes, -- comments and
// $$ bodies ends a statement, except inside a SQLite trigger body
// (CREATE TRIGGER ... BEGIN ... END;).
std::vector<std::string> split_sql(std::string_view sql);

// Applies pending migrations in order, one transaction each, recording
// version, description and SHA-256 checksum in schema_version (section 11.4).
// A recorded checksum that differs from the embedded file is a Config error.
// With apply = false, any pending migration is a Config error instead.
Result<std::vector<AppliedMigration>> migrate(Db& db, bool apply);

}  // namespace pychron::persistence::detail
