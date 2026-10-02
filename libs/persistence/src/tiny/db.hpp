#pragma once

// The TinyORM boundary. Everything that touches Qt or TinyORM types lives
// under src/; public headers stay Qt-free. Driver exceptions are caught here
// and become Result errors (DVC schema spec, section 12.2).

#include <QByteArray>
#include <QList>
#include <QString>
#include <QVariant>
#include <QVariantMap>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/persistence/ids.hpp"
#include "pychron/persistence/store.hpp"

namespace Orm {
class DatabaseConnection;
}

namespace pychron::persistence::detail {

using Row = QVariantMap;
using Bindings = QList<QVariant>;

class Db {
 public:
  static Result<std::unique_ptr<Db>> open(const StoreConfig& config);
  ~Db();
  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;

  Dialect dialect() const noexcept { return dialect_; }

  // One statement, no bindings (DDL, BEGIN/COMMIT).
  Result<void> unprepared(const QString& sql);
  Result<std::vector<Row>> select(const QString& sql, const Bindings& bindings = {});
  Result<std::optional<Row>> select_one(const QString& sql, const Bindings& bindings = {});
  // Rows affected by an INSERT/UPDATE/DELETE.
  Result<int> affecting(const QString& sql, const Bindings& bindings = {});
  // TinyORM query builder inserts.
  Result<void> insert(const QString& table, const Row& row);
  Result<void> insert_many(const QString& table, const QList<QVariantMap>& rows);
  Result<int> insert_or_ignore(const QString& table, const Row& row);

  // Write transaction: BEGIN IMMEDIATE on SQLite (takes the writer lock up
  // front, section 11.3), BEGIN (READ COMMITTED) on PostgreSQL.
  Result<void> begin_write();
  Result<void> commit();
  void rollback() noexcept;

 private:
  Db(QString connection_name, Dialect dialect);
  Orm::DatabaseConnection& conn();
  // Runs `fn` with driver exceptions mapped to Error. After a lost connection
  // the next call reconnects first (outside a transaction only): TinyORM's
  // own retry re-executes a stale QPSQL prepared statement and fails.
  template <class Fn>
  auto run(Fn&& fn) -> Result<decltype(fn())>;

  QString name_;
  Dialect dialect_;
  bool reconnect_ = false;  // the connection was lost; reconnect before the next statement
  bool in_tx_ = false;      // a write transaction is open (begin_write .. commit/rollback)
};

// RAII write transaction: rolls back unless commit() succeeded.
class WriteTx {
 public:
  explicit WriteTx(Db& db) : db_(db) {}
  ~WriteTx() {
    if (open_) db_.rollback();
  }
  WriteTx(const WriteTx&) = delete;
  WriteTx& operator=(const WriteTx&) = delete;

  Result<void> begin() {
    auto r = db_.begin_write();
    open_ = r.has_value();
    return r;
  }
  Result<void> commit() {
    auto r = db_.commit();
    open_ = false;
    if (!r) db_.rollback();
    return r;
  }
  void rollback() noexcept {
    if (open_) db_.rollback();
    open_ = false;
  }

 private:
  Db& db_;
  bool open_ = false;
};

// ---------------------------------------------------------------- conversions

inline QString qs(const std::string& s) { return QString::fromStdString(s); }
inline QVariant qv(const std::string& s) { return QVariant(qs(s)); }
inline QVariant qv(const char* s) { return QVariant(QString::fromUtf8(s)); }
inline QVariant qv(const Uuid& u) { return QVariant(qs(u.str())); }
inline QVariant qv(const UtcTime& t) { return QVariant(qs(t.iso())); }
inline QVariant qv(double d) { return QVariant(d); }
inline QVariant qv(int i) { return QVariant(i); }
inline QVariant qv(long long i) { return QVariant(static_cast<qlonglong>(i)); }
inline QVariant qv(long i) { return QVariant(static_cast<qlonglong>(i)); }
inline QVariant qv(bool b) { return QVariant(b); }
inline QVariant qv(const Sha256Digest& d) {
  return QVariant(QByteArray(reinterpret_cast<const char*>(d.data()), static_cast<qsizetype>(d.size())));
}
inline QVariant qv(const Bytes& b) {
  return QVariant(QByteArray(reinterpret_cast<const char*>(b.data()), static_cast<qsizetype>(b.size())));
}
template <class T>
QVariant qv(const std::optional<T>& o) {
  return o ? qv(*o) : QVariant();
}

std::string to_std(const QVariant& v);
std::optional<std::string> opt_str(const QVariant& v);
std::optional<double> opt_double(const QVariant& v);
std::optional<int> opt_int(const QVariant& v);
std::optional<bool> opt_bool(const QVariant& v);
Uuid to_uuid(const QVariant& v);
std::optional<Uuid> opt_uuid(const QVariant& v);
UtcTime to_time(const QVariant& v);
Sha256Digest to_digest(const QVariant& v);

}  // namespace pychron::persistence::detail
