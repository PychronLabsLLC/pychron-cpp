#include "tiny/db.hpp"

#include <orm/concerns/detectslostconnections.hpp>
#include <orm/databaseconnection.hpp>
#include <orm/databasemanager.hpp>
#include <orm/exceptions/sqlerror.hpp>
#include <orm/query/querybuilder.hpp>

#include <QCoreApplication>
#include <QSqlError>
#include <QSqlRecord>
#include <QUrl>
#include <QUrlQuery>

#include <atomic>
#include <cstring>
#include <exception>
#include <mutex>

#include "sql/errors.hpp"

namespace pychron::persistence::detail {
namespace {

std::shared_ptr<Orm::DatabaseManager> manager() {
  static std::mutex mutex;
  std::lock_guard lock(mutex);
  try {
    return Orm::DatabaseManager::instance();
  } catch (const std::exception&) {
    return Orm::DatabaseManager::create();
  }
}

// QtSql loads its drivers through the plugin loader, which needs a
// QCoreApplication: without one, newer Qt (6.5+) hands back a driverless
// QSqlDatabase and opening it dereferences null. Hosts that are not Qt
// applications (elctl, the tests) get a private instance here; a Qt host's
// own instance is left alone. Never destroyed: connections may outlive main().
void ensure_qt_application() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (QCoreApplication::instance() != nullptr) return;
    static int argc = 1;
    static char name[] = "pychron";
    static char* argv[] = {name, nullptr};
    new QCoreApplication(argc, argv);
  });
}

QString next_connection_name() {
  static std::atomic<int> counter{0};
  return QStringLiteral("pychron-%1").arg(counter.fetch_add(1));
}

// Runs `fn`, turning any TinyORM/Qt SQL exception into an Error.
template <class Fn>
auto guarded(Dialect dialect, Fn&& fn) -> Result<decltype(fn())> {
  try {
    if constexpr (std::is_void_v<decltype(fn())>) {
      fn();
      return {};
    } else {
      return fn();
    }
  } catch (const Orm::Exceptions::SqlError& e) {
    const QSqlError& err = e.getSqlError();
    const bool lost = err.type() == QSqlError::ConnectionError ||
                      Orm::Concerns::DetectsLostConnections::causedByLostConnection(err.databaseText());
    return fail(sql_error(dialect, err.nativeErrorCode().toStdString(), lost, e.what()));
  } catch (const std::exception& e) {
    return fail(ErrorKind::Io, e.what());
  }
}

Result<QVariantHash> parse_config(const StoreConfig& config, Dialect& dialect) {
  const std::string& url = config.url;
  QVariantHash c;
  if (url.rfind("sqlite:", 0) == 0) {
    dialect = Dialect::Sqlite;
    const std::string path = url.substr(std::strlen("sqlite:"));
    if (path.empty()) return fail(ErrorKind::Config, "store url: empty sqlite path");
    c["driver"] = "QSQLITE";
    c["database"] = qs(path);
    c["foreign_key_constraints"] = true;
    c["check_database_exists"] = false;
    c["options"] = "QSQLITE_BUSY_TIMEOUT=10000";
    return c;
  }
  if (url.rfind("postgresql://", 0) == 0 || url.rfind("postgres://", 0) == 0) {
    dialect = Dialect::PostgreSql;
    const QUrl u(qs(url));
    if (!u.isValid() || u.host().isEmpty()) return fail(ErrorKind::Config, "store url: invalid postgresql url");
    c["driver"] = "QPSQL";
    c["host"] = u.host();
    c["port"] = u.port(5432);
    c["database"] = u.path().mid(1);
    c["username"] = u.userName(QUrl::FullyDecoded);
    c["password"] = u.password(QUrl::FullyDecoded);
    c["charset"] = "utf8";
    c["search_path"] = "public";
    // search_path=<schema> confines a store to one schema (tests use a fresh one each).
    c["application_name"] = "pychron";
    const QUrlQuery q(u);
    for (const char* key : {"sslmode", "sslcert", "sslkey", "sslrootcert", "search_path"})
      if (q.hasQueryItem(key)) c[key] = q.queryItemValue(key, QUrl::FullyDecoded);
    return c;
  }
  return fail(ErrorKind::Config, "store url must start with sqlite: or postgresql://");
}

}  // namespace

Db::Db(QString connection_name, Dialect dialect) : name_(std::move(connection_name)), dialect_(dialect) {}

Db::~Db() {
  try {
    manager()->removeConnection(name_);
  } catch (...) {
  }
}

Orm::DatabaseConnection& Db::conn() { return manager()->connection(name_); }

template <class Fn>
auto Db::run(Fn&& fn) -> Result<decltype(fn())> {
  if (reconnect_) {
    // The transaction died with the old session; never continue it on a new one.
    if (in_tx_) return fail(ErrorKind::NotConnected, "connection lost inside a transaction", "persistence");
    // reconnect() also resets TinyORM's transaction state.
    auto r = guarded(dialect_, [&] { manager()->reconnect(name_); });
    if (!r) return fail(r.error());
    reconnect_ = false;
  }
  auto result = guarded(dialect_, std::forward<Fn>(fn));
  if (!result && result.error().kind == ErrorKind::NotConnected) reconnect_ = true;
  return result;
}

Result<std::unique_ptr<Db>> Db::open(const StoreConfig& config) {
  Dialect dialect = Dialect::Sqlite;
  auto cfg = parse_config(config, dialect);
  if (!cfg) return fail(cfg.error());
  ensure_qt_application();
  const QString name = next_connection_name();
  auto added = guarded(dialect, [&] { manager()->addConnection(*cfg, name); });
  if (!added) return fail(added.error());
  std::unique_ptr<Db> db(new Db(name, dialect));
  auto connected = guarded(dialect, [&] { db->conn().connectEagerly(); });
  if (!connected) return fail(connected.error());
  return db;
}

Result<void> Db::unprepared(const QString& sql) {
  return run([&] { conn().unprepared(sql); });
}

Result<std::vector<Row>> Db::select(const QString& sql, const Bindings& bindings) {
  return run([&] {
    auto q = conn().select(sql, bindings);
    std::vector<Row> rows;
    const QSqlRecord rec = q.record();
    while (q.next()) {
      Row r;
      for (int i = 0; i < rec.count(); ++i) r.insert(rec.fieldName(i), q.value(i));
      rows.push_back(std::move(r));
    }
    return rows;
  });
}

Result<std::optional<Row>> Db::select_one(const QString& sql, const Bindings& bindings) {
  auto rows = select(sql, bindings);
  if (!rows) return fail(rows.error());
  if (rows->empty()) return std::optional<Row>{};
  return std::optional<Row>{std::move(rows->front())};
}

Result<int> Db::affecting(const QString& sql, const Bindings& bindings) {
  return run([&] { return std::get<0>(conn().affectingStatement(sql, bindings)); });
}

Result<void> Db::insert(const QString& table, const Row& row) {
  return run([&] { conn().table(table)->insert(row); });
}

Result<void> Db::insert_many(const QString& table, const QList<QVariantMap>& rows) {
  if (rows.isEmpty()) return {};
  return run([&] { conn().table(table)->insert(rows); });
}

Result<int> Db::insert_or_ignore(const QString& table, const Row& row) {
  return run([&] { return std::get<0>(conn().table(table)->insertOrIgnore(row)); });
}

// PostgreSQL transactions go through TinyORM so it knows one is open: when the
// connection drops inside a transaction it then rethrows instead of silently
// reconnecting and re-running the statement in autocommit on a new session,
// which would commit half a changeset. SQLite needs BEGIN IMMEDIATE, which
// TinyORM cannot issue; a local file connection is never "lost", so its
// reconnect path does not apply.
Result<void> Db::begin_write() {
  auto r = dialect_ == Dialect::Sqlite ? unprepared(QStringLiteral("BEGIN IMMEDIATE"))
                                       : run([&] { conn().beginTransaction(); });
  in_tx_ = r.has_value();
  return r;
}

Result<void> Db::commit() {
  // A COMMIT sent to a fresh session would "succeed" with a warning, so a
  // transaction whose connection was lost must fail here.
  auto r = dialect_ == Dialect::Sqlite ? unprepared(QStringLiteral("COMMIT")) : run([&] { conn().commit(); });
  // A failed COMMIT (e.g. a deferred foreign key on SQLite) can leave the
  // transaction open; it stays ours until rollback().
  if (r) in_tx_ = false;
  return r;
}

void Db::rollback() noexcept {
  try {
    if (in_tx_ && !reconnect_) {
      if (dialect_ == Dialect::Sqlite)
        (void)unprepared(QStringLiteral("ROLLBACK"));
      else if (conn().inTransaction())
        (void)run([&] { conn().rollBack(); });
    }
  } catch (...) {
  }
  in_tx_ = false;
}

// ---------------------------------------------------------------- conversions

std::string to_std(const QVariant& v) { return v.toString().toStdString(); }

std::optional<std::string> opt_str(const QVariant& v) {
  if (v.isNull()) return std::nullopt;
  return v.toString().toStdString();
}

std::optional<double> opt_double(const QVariant& v) {
  if (v.isNull()) return std::nullopt;
  return v.toDouble();
}

std::optional<int> opt_int(const QVariant& v) {
  if (v.isNull()) return std::nullopt;
  return v.toInt();
}

std::optional<bool> opt_bool(const QVariant& v) {
  if (v.isNull()) return std::nullopt;
  return v.toBool();
}

Uuid to_uuid(const QVariant& v) { return Uuid::parse(v.toString().toStdString()).value_or(Uuid{}); }

std::optional<Uuid> opt_uuid(const QVariant& v) {
  if (v.isNull()) return std::nullopt;
  return Uuid::parse(v.toString().toStdString());
}

UtcTime to_time(const QVariant& v) { return UtcTime::parse(v.toString().toStdString()).value_or(UtcTime{}); }

Sha256Digest to_digest(const QVariant& v) {
  Sha256Digest d{};
  const QByteArray b = v.toByteArray();
  if (b.size() == static_cast<qsizetype>(d.size())) std::memcpy(d.data(), b.constData(), d.size());
  return d;
}

}  // namespace pychron::persistence::detail
