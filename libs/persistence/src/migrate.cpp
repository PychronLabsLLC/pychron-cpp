#include "migrate.hpp"

#include <algorithm>
#include <cctype>
#include <map>

#include "pychron/core/sha256.hpp"
#include "sql/statements.hpp"

namespace pychron::persistence::detail {
namespace {

std::string upper_trimmed(std::string_view s) {
  std::string out;
  for (char c : s) out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  const auto first = out.find_first_not_of(" \t\r\n");
  const auto last = out.find_last_not_of(" \t\r\n");
  return first == std::string::npos ? std::string{} : out.substr(first, last - first + 1);
}

bool has_word(const std::string& upper, std::string_view word) {
  for (auto pos = upper.find(word); pos != std::string::npos; pos = upper.find(word, pos + 1)) {
    const bool left = pos == 0 || !std::isalnum(static_cast<unsigned char>(upper[pos - 1]));
    const auto end = pos + word.size();
    const bool right = end >= upper.size() || !std::isalnum(static_cast<unsigned char>(upper[end]));
    if (left && right) return true;
  }
  return false;
}

// "0001_init.sql" -> {1, "init"}
bool parse_file_name(std::string_view file, int& version, std::string& description) {
  if (file.size() < 10 || file.substr(4, 1) != "_" || file.substr(file.size() - 4) != ".sql") return false;
  version = 0;
  for (char c : file.substr(0, 4)) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    version = version * 10 + (c - '0');
  }
  description = std::string(file.substr(5, file.size() - 9));
  return version > 0;
}

}  // namespace

std::vector<std::string> split_sql(std::string_view sql) {
  std::vector<std::string> out;
  std::string buf;
  bool in_quote = false;
  bool in_dollar = false;
  for (std::size_t i = 0; i < sql.size(); ++i) {
    const char c = sql[i];
    if (!in_quote && !in_dollar && c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
      while (i < sql.size() && sql[i] != '\n') ++i;
      buf.push_back('\n');
      continue;
    }
    if (!in_quote && c == '$' && i + 1 < sql.size() && sql[i + 1] == '$') {
      in_dollar = !in_dollar;
      buf += "$$";
      ++i;
      continue;
    }
    if (c == '\'' && !in_dollar) in_quote = !in_quote;
    if (c == ';' && !in_quote && !in_dollar) {
      const std::string upper = upper_trimmed(buf);
      const bool trigger_body = upper.rfind("CREATE TRIGGER", 0) == 0 && has_word(upper, "BEGIN");
      if (trigger_body && !(upper.size() >= 3 && upper.compare(upper.size() - 3, 3, "END") == 0)) {
        buf.push_back(c);
        continue;
      }
      if (!upper.empty()) {
        const auto first = buf.find_first_not_of(" \t\r\n");
        out.push_back(buf.substr(first, buf.find_last_not_of(" \t\r\n") - first + 1));
      }
      buf.clear();
      continue;
    }
    buf.push_back(c);
  }
  if (!upper_trimmed(buf).empty()) {
    const auto first = buf.find_first_not_of(" \t\r\n");
    out.push_back(buf.substr(first, buf.find_last_not_of(" \t\r\n") - first + 1));
  }
  return out;
}

Result<std::vector<AppliedMigration>> migrate(Db& db, bool apply) {
  const char* dialect_dir = db.dialect() == Dialect::PostgreSql ? "pg" : "sqlite";

  struct Pending {
    int version;
    std::string description;
    std::string_view text;
    Sha256Digest checksum;
  };
  std::vector<Pending> known;
  for (std::size_t i = 0; i < kEmbeddedMigrationCount; ++i) {
    const auto& m = kEmbeddedMigrations[i];
    if (std::string_view(m.dialect) != dialect_dir) continue;
    Pending p{};
    if (!parse_file_name(m.file, p.version, p.description))
      return fail(ErrorKind::Config, std::string("migration file name must be NNNN_<name>.sql: ") + m.file);
    p.text = std::string_view(reinterpret_cast<const char*>(m.data), m.size);
    p.checksum = sha256(p.text);
    known.push_back(std::move(p));
  }
  std::sort(known.begin(), known.end(), [](const Pending& a, const Pending& b) { return a.version < b.version; });

  auto read_applied = [&]() -> Result<std::map<int, AppliedMigration>> {
    std::map<int, AppliedMigration> applied;
    auto exists = db.select_one(sql::schema_version_exists(db.dialect()));
    if (!exists) return fail(exists.error());
    if (!*exists || (*exists)->value("n").toInt() == 0) return applied;
    auto rows = db.select(sql::kSelectSchemaVersions);
    if (!rows) return fail(rows.error());
    for (const auto& r : *rows) {
      const QByteArray sum = r.value("checksum").toByteArray();
      applied[r.value("version").toInt()] = AppliedMigration{
          r.value("version").toInt(), to_std(r.value("description")),
          to_hex(std::span(reinterpret_cast<const std::uint8_t*>(sum.constData()), static_cast<std::size_t>(sum.size())))};
    }
    return applied;
  };

  for (const auto& m : known) {
    WriteTx tx(db);
    if (auto r = tx.begin(); !r) return fail(r.error());
    // Serialise concurrent migrators on PostgreSQL; SQLite's BEGIN IMMEDIATE
    // already holds the database writer lock.
    if (db.dialect() == Dialect::PostgreSql)
      if (auto r = db.select(sql::kPgMigrationLock); !r) return fail(r.error());
    auto applied = read_applied();
    if (!applied) return fail(applied.error());
    const std::string checksum_hex = to_hex(m.checksum);
    if (auto it = applied->find(m.version); it != applied->end()) {
      if (it->second.checksum_hex != checksum_hex)
        return fail(ErrorKind::Config, "schema_version " + std::to_string(m.version) +
                                           ": checksum differs from the migration this build carries");
      continue;
    }
    if (!apply)
      return fail(ErrorKind::Config, "database schema is out of date (migration " + std::to_string(m.version) + " " +
                                         m.description +
                                         " not applied); bring it up to date with: elctl db migrate --db <url>");
    for (const auto& statement : split_sql(m.text))
      if (auto r = db.unprepared(qs(statement)); !r) return fail(r.error());
    Row row;
    row["version"] = m.version;
    row["description"] = qs(m.description);
    row["checksum"] = qv(m.checksum);
    row["applied_utc"] = qv(UtcTime::now());
    if (auto r = db.insert("schema_version", row); !r) return fail(r.error());
    if (auto r = tx.commit(); !r) return fail(r.error());
  }

  auto applied = read_applied();
  if (!applied) return fail(applied.error());
  std::vector<AppliedMigration> out;
  for (auto& [v, m] : *applied) out.push_back(std::move(m));
  return out;
}

}  // namespace pychron::persistence::detail
