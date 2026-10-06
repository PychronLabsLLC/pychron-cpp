// Entry reads, the catalog edit batch and identifier allocation (sample and
// package entry spec, section 5).

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <set>

#include "catalog_impl.hpp"
#include "pychron/core/calendar.hpp"
#include "sql/catalog.hpp"
#include "sql/errors.hpp"
#include "sql/geometry.hpp"
#include "sql/statements.hpp"

namespace pychron::persistence {

std::string_view table_name(CatalogTable table) noexcept {
  switch (table) {
    case CatalogTable::PrincipalInvestigator: return "principal_investigator";
    case CatalogTable::Project: return "project";
    case CatalogTable::Material: return "material";
    case CatalogTable::Sample: return "sample";
    case CatalogTable::Irradiation: return "irradiation";
    case CatalogTable::Level: return "level";
    case CatalogTable::IrradiationPosition: return "irradiation_position";
    case CatalogTable::User: return "app_user";
    case CatalogTable::MassSpectrometer: return "mass_spectrometer";
    case CatalogTable::ExtractDevice: return "extract_device";
    case CatalogTable::Load: return "load";
    case CatalogTable::LoadPosition: return "load_position";
    case CatalogTable::Repository: return "repository";
    case CatalogTable::RefObject: return "ref_object";
    case CatalogTable::Identifier: return "identifier";
  }
  return "";
}

namespace detail {
namespace {

QString qstr(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

// ---------------------------------------------------------------- the editable columns (5.2)

// Latitude and Longitude are the two halves of the sample's `geom` point
// (sql/geometry.hpp): editable as "lat" and "lon", stored as one column.
enum class ColumnType { Text, Real, Int, Bool, Id, Date, Latitude, Longitude };

constexpr const char* kGeomColumn = "geom";

bool is_geo(ColumnType t) { return t == ColumnType::Latitude || t == ColumnType::Longitude; }

struct Column {
  const char* name;
  ColumnType type;
  bool required;  // NOT NULL without a default: an insert must give it
  bool not_null;  // never NULL (required, or NOT NULL with a default)
};

struct TableRules {
  CatalogTable table;
  std::vector<Column> columns;
  std::vector<const char*> key;  // the natural key (UNIQUE)
  bool insert = true, update = true, remove = true;
};

const std::vector<TableRules>& rules() {
  using T = ColumnType;
  static const std::vector<TableRules> all = {
      {CatalogTable::PrincipalInvestigator,
       {{"last_name", T::Text, true, true},
        {"first_initial", T::Text, false, true},
        {"affiliation", T::Text, false, false},
        {"email", T::Text, false, false}},
       {"last_name", "first_initial"}},
      {CatalogTable::Project,
       {{"name", T::Text, true, true},
        {"pi_uuid", T::Id, false, false},
        {"checkin_date", T::Date, false, false},
        {"comment", T::Text, false, false},
        {"lab_contact", T::Text, false, false},
        {"institution", T::Text, false, false}},
       {"name", "pi_uuid"}},
      {CatalogTable::Material,
       {{"name", T::Text, true, true}, {"grainsize", T::Text, false, true}},
       {"name", "grainsize"}},
      {CatalogTable::Sample,
       {{"name", T::Text, true, true},
        {"project_uuid", T::Id, true, true},
        {"material_uuid", T::Id, true, true},
        {"note", T::Text, false, false},
        {"igsn", T::Text, false, false},
        {"lat", T::Latitude, false, false},
        {"lon", T::Longitude, false, false},
        {"elevation", T::Real, false, false},
        {"storage_location", T::Text, false, false},
        {"location", T::Text, false, false},
        {"unit", T::Text, false, false},
        {"lithology", T::Text, false, false},
        {"lithology_class", T::Text, false, false},
        {"lithology_type", T::Text, false, false},
        {"lithology_group", T::Text, false, false},
        {"approximate_age", T::Real, false, false}},
       {"name", "project_uuid", "material_uuid"}},
      {CatalogTable::Irradiation,
       {{"name", T::Text, true, true}, {"kind", T::Text, false, true}},
       {"name"}},
      {CatalogTable::Level,
       {{"irradiation_uuid", T::Id, true, true},
        {"name", T::Text, true, true},
        {"holder_ref_uuid", T::Id, false, false},
        {"note", T::Text, false, false}},
       {"irradiation_uuid", "name"}},
      {CatalogTable::IrradiationPosition,
       {{"level_uuid", T::Id, true, true},
        {"position", T::Int, true, true},
        {"sample_uuid", T::Id, false, false},
        {"weight", T::Real, false, false},
        {"packet", T::Text, false, false},
        {"note", T::Text, false, false}},
       {"level_uuid", "position"}},
      // Identifiers are made by allocate_identifiers only.
      {CatalogTable::Identifier,
       {{"identifier", T::Text, true, true}, {"position_uuid", T::Id, false, false}},
       {"identifier"},
       false},
      // Reference objects entry creates with a package, a level or a holder;
      // their values are revisions. Keys change only with a rename (E11).
      {CatalogTable::RefObject,
       {{"ref_type", T::Text, true, true},
        {"key", T::Text, true, true},
        {"irradiation_uuid", T::Id, false, false},
        {"level_uuid", T::Id, false, false},
        {"position_uuid", T::Id, false, false}},
       {"ref_type", "key"},
       true,
       false,
       false},
  };
  return all;
}

const TableRules* rules_of(CatalogTable table) {
  for (const auto& r : rules())
    if (r.table == table) return &r;
  return nullptr;
}

const Column* column_of(const TableRules& t, const std::string& name) {
  for (const auto& c : t.columns)
    if (name == c.name) return &c;
  return nullptr;
}

std::string describe(const CatalogValue& v) {
  struct {
    std::string operator()(std::monostate) const { return "null"; }
    std::string operator()(const std::string& s) const { return "'" + s + "'"; }
    std::string operator()(double d) const { return std::to_string(d); }
    std::string operator()(std::int64_t i) const { return std::to_string(i); }
    std::string operator()(bool b) const { return b ? "true" : "false"; }
    std::string operator()(const Uuid& u) const { return u.str(); }
  } visitor;
  return std::visit(visitor, v);
}

// A value of the column's type (or null where the column allows it); Error otherwise.
Result<void> check_value(const TableRules& t, const Column& c, const CatalogValue& v) {
  const std::string where = std::string(table_name(t.table)) + "." + c.name;
  if (std::holds_alternative<std::monostate>(v)) {
    if (c.not_null) return fail(ErrorKind::Protocol, where + " cannot be null");
    return {};
  }
  bool ok = false;
  switch (c.type) {
    case ColumnType::Text:
      ok = std::holds_alternative<std::string>(v);
      break;
    case ColumnType::Date:
      ok = std::holds_alternative<std::string>(v) && is_calendar_date(std::get<std::string>(v));
      break;
    case ColumnType::Real:
      ok = (std::holds_alternative<double>(v) && std::isfinite(std::get<double>(v))) ||
           std::holds_alternative<std::int64_t>(v);
      break;
    case ColumnType::Latitude:
    case ColumnType::Longitude: {
      const double limit = c.type == ColumnType::Latitude ? 90.0 : 180.0;
      std::optional<double> d;
      if (const auto* x = std::get_if<double>(&v)) d = *x;
      if (const auto* i = std::get_if<std::int64_t>(&v)) d = static_cast<double>(*i);
      ok = d && std::isfinite(*d) && std::fabs(*d) <= limit;
      if (d && !ok)
        return fail(ErrorKind::Protocol, where + ": " + describe(v) + " is not in " +
                                             (c.type == ColumnType::Latitude ? "[-90, 90]" : "[-180, 180]"));
      break;
    }
    case ColumnType::Int:
      ok = std::holds_alternative<std::int64_t>(v);
      break;
    case ColumnType::Bool:
      ok = std::holds_alternative<bool>(v);
      break;
    case ColumnType::Id:
      ok = std::holds_alternative<Uuid>(v);
      break;
  }
  if (!ok) return fail(ErrorKind::Protocol, where + ": " + describe(v) + " is not a value of this column");
  if (t.table == CatalogTable::RefObject && std::string(c.name) == "ref_type") {
    const auto& type = std::get<std::string>(v);
    static const char* const kEntryTypes[] = {"level_geometry", "level_production", "production", "chronology",
                                              "irradiation_holder", "document"};
    if (std::none_of(std::begin(kEntryTypes), std::end(kEntryTypes), [&](const char* k) { return type == k; }))
      return fail(ErrorKind::Protocol, where + ": entry does not create " + describe(v) + " references");
  }
  if (t.table == CatalogTable::Irradiation && std::string(c.name) == "kind") {
    const auto& kind = std::get<std::string>(v);
    if (kind != "irradiation" && kind != "package")
      return fail(ErrorKind::Protocol, where + ": " + describe(v) + " is not irradiation or package");
  }
  return {};
}

QVariant to_variant(const CatalogValue& v) {
  struct {
    QVariant operator()(std::monostate) const { return QVariant(); }
    QVariant operator()(const std::string& s) const { return qv(s); }
    QVariant operator()(double d) const { return QVariant(d); }
    QVariant operator()(std::int64_t i) const { return QVariant(static_cast<qlonglong>(i)); }
    QVariant operator()(bool b) const { return QVariant(b); }
    QVariant operator()(const Uuid& u) const { return qv(u); }
  } visitor;
  return std::visit(visitor, v);
}

// A real column given as an integer compares as the double it is stored as.
CatalogValue normalized(const Column& c, const CatalogValue& v) {
  if (c.type == ColumnType::Real || is_geo(c.type))
    if (const auto* i = std::get_if<std::int64_t>(&v)) return static_cast<double>(*i);
  return v;
}

CatalogValue from_variant(const Column& c, const QVariant& v) {
  if (v.isNull()) return std::monostate{};
  switch (c.type) {
    case ColumnType::Text:
    case ColumnType::Date:
      return to_std(v);
    case ColumnType::Real:
    case ColumnType::Latitude:
    case ColumnType::Longitude:
      return v.toDouble();
    case ColumnType::Int:
      return static_cast<std::int64_t>(v.toLongLong());
    case ColumnType::Bool:
      return v.toBool();
    case ColumnType::Id:
      return to_uuid(v);
  }
  return std::monostate{};
}

QJsonValue json_of(const CatalogValue& v) {
  struct {
    QJsonValue operator()(std::monostate) const { return QJsonValue(QJsonValue::Null); }
    QJsonValue operator()(const std::string& s) const { return QJsonValue(qs(s)); }
    QJsonValue operator()(double d) const { return QJsonValue(d); }
    QJsonValue operator()(std::int64_t i) const { return QJsonValue(static_cast<qint64>(i)); }
    QJsonValue operator()(bool b) const { return QJsonValue(b); }
    QJsonValue operator()(const Uuid& u) const { return QJsonValue(qs(u.str())); }
  } visitor;
  return std::visit(visitor, v);
}

std::string compact(const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString(); }

// ---------------------------------------------------------------- the geometry column

// The SELECT list of a table's editable columns: every stored column quoted,
// and the point the geo columns share read once, as `geom`.
QString select_list(const TableRules& t, Dialect dialect) {
  QStringList names;
  bool geo = false;
  for (const auto& c : t.columns) {
    if (is_geo(c.type))
      geo = true;
    else
      names << QStringLiteral("\"%1\"").arg(QString::fromUtf8(c.name));
  }
  if (geo) names << geom_read(dialect, QString::fromUtf8(kGeomColumn)) + QStringLiteral(" AS geom");
  return names.join(QStringLiteral(", "));
}

// The editable fields of a selected row (select_list's columns).
CatalogFields fields_of(const TableRules& t, const Row& row) {
  CatalogFields out;
  std::optional<GeoPoint> point;
  bool parsed = false;
  for (const auto& c : t.columns) {
    if (!is_geo(c.type)) {
      out[c.name] = from_variant(c, row.value(QString::fromUtf8(c.name)));
      continue;
    }
    if (!parsed) {
      const QVariant geom = row.value(QString::fromUtf8(kGeomColumn));
      if (!geom.isNull()) point = parse_point(to_std(geom));
      parsed = true;
    }
    if (!point)
      out[c.name] = std::monostate{};
    else
      out[c.name] = c.type == ColumnType::Latitude ? point->lat : point->lon;
  }
  return out;
}

// The geom cell for a row's lat and lon (normalized values): NULL without
// either, the point with both; half a point is an error.
Result<QVariant> geom_cell(const TableRules& t, const CatalogFields& row) {
  std::optional<double> lat, lon;
  for (const auto& c : t.columns) {
    if (!is_geo(c.type)) continue;
    const auto it = row.find(c.name);
    if (it == row.end()) continue;
    if (const auto* d = std::get_if<double>(&it->second)) (c.type == ColumnType::Latitude ? lat : lon) = *d;
  }
  if (lat.has_value() != lon.has_value())
    return fail(ErrorKind::Protocol, std::string(table_name(t.table)) + ": latitude and longitude go together");
  if (!lat) return QVariant();
  return qv(ewkt_point(*lat, *lon));
}

bool has_geo(const TableRules& t) {
  return std::any_of(t.columns.begin(), t.columns.end(), [](const Column& c) { return is_geo(c.type); });
}

// ---------------------------------------------------------------- the batch

class EditRun {
 public:
  EditRun(Db& db, const CatalogEditBatch& batch) : db_(db), batch_(batch) {}

  // Applies every edit inside the caller's transaction. Infrastructure
  // failures are errors; stale rows and refusals are collected.
  Result<void> run() {
    for (const auto& edit : batch_.edits) {
      Result<void> r = std::visit([&](const auto& e) { return apply(e); }, edit);
      if (!r) return r;
      if (fatal_) break;  // a statement failed in a way that ends the transaction
    }
    return {};
  }

  bool clean() const { return stale_.empty() && refused_.empty(); }
  std::vector<StaleRow>& stale() { return stale_; }
  std::vector<Refusal>& refused() { return refused_; }
  // One change_entity row per row touched (the key is change_seq, type,
  // uuid): edits of one row in a batch merge into one diff.
  std::vector<ChangeEntityRow> entities() const {
    std::vector<ChangeEntityRow> out;
    for (const auto& a : audit_)
      out.push_back(ChangeEntityRow{qstr(table_name(a.table)), a.uuid, a.op, compact(a.diff)});
    return out;
  }

 private:
  // ------------------------------------------------------------ validation

  Result<const TableRules*> table(CatalogTable t, const char* op) {
    const TableRules* rules = rules_of(t);
    if (!rules) return fail(ErrorKind::Protocol, std::string(op) + ": " + std::string(table_name(t)) + " is not editable");
    return rules;
  }

  Result<void> check_fields(const TableRules& t, const CatalogFields& fields) {
    for (const auto& [name, value] : fields) {
      const Column* c = column_of(t, name);
      if (!c)
        return fail(ErrorKind::Protocol, std::string(table_name(t.table)) + "." + name + " is not an editable column");
      if (auto r = check_value(t, *c, value); !r) return r;
    }
    return {};
  }

  // ------------------------------------------------------------ reads

  // The row's editable columns, locked for this transaction on PostgreSQL.
  Result<std::optional<CatalogFields>> current(const TableRules& t, Uuid uuid) {
    QString sql = QStringLiteral("SELECT %1 FROM %2 WHERE uuid = ?")
                      .arg(select_list(t, db_.dialect()), qstr(table_name(t.table)));
    if (db_.dialect() == Dialect::PostgreSql) sql += QStringLiteral(" FOR UPDATE");
    auto row = db_.select_one(sql, {qv(uuid)});
    if (!row) return fail(row.error());
    if (!*row) return std::optional<CatalogFields>{};
    return std::optional<CatalogFields>{fields_of(t, **row)};
  }

  Result<std::int64_t> count(const QString& sql, const Bindings& bindings) {
    auto row = db_.select_one(sql, bindings);
    if (!row) return fail(row.error());
    if (!*row) return std::int64_t{0};
    return static_cast<std::int64_t>((*row)->first().toLongLong());
  }

  // ------------------------------------------------------------ rules

  void refuse(CatalogTable t, Uuid uuid, std::string rule, std::string what) {
    refused_.push_back(Refusal{t, uuid, std::move(rule), std::move(what)});
  }

  // Another row already has the natural key `row` would have.
  Result<bool> unique_clash(const TableRules& t, Uuid uuid, const CatalogFields& row) {
    QString sql = QStringLiteral("SELECT uuid FROM %1 WHERE uuid <> ?").arg(qstr(table_name(t.table)));
    Bindings b{qv(uuid)};
    for (const char* k : t.key) {
      const auto it = row.find(k);
      const CatalogValue v = it == row.end() ? CatalogValue{} : it->second;
      if (std::holds_alternative<std::monostate>(v)) {
        sql += QStringLiteral(" AND \"%1\" IS NULL").arg(QString::fromUtf8(k));
      } else {
        sql += QStringLiteral(" AND \"%1\" = ?").arg(QString::fromUtf8(k));
        b << to_variant(v);
      }
    }
    auto found = db_.select_one(sql, b);
    if (!found) return fail(found.error());
    if (!found->has_value()) return false;
    std::string key;
    for (const char* k : t.key) {
      const auto it = row.find(k);
      if (!key.empty()) key += ", ";
      key += std::string(k) + " = " + describe(it == row.end() ? CatalogValue{} : it->second);
    }
    refuse(t.table, uuid, "unique", std::string(table_name(t.table)) + " with " + key + " exists");
    return true;
  }

  // The identifier is analyzed, loaded or leased.
  Result<bool> identifier_in_use(Uuid identifier, std::string* why) {
    auto row = db_.select_one(sql::kIdentifierUse, {qv(identifier), qv(identifier), qv(identifier)});
    if (!row) return fail(row.error());
    const auto n_analyses = (*row)->value("n_analyses").toLongLong();
    const auto n_loads = (*row)->value("n_loads").toLongLong();
    const auto n_leases = (*row)->value("n_leases").toLongLong();
    if (n_analyses == 0 && n_loads == 0 && n_leases == 0) return false;
    *why = std::to_string(n_analyses) + " analyses, " + std::to_string(n_loads) + " load positions, " +
           std::to_string(n_leases) + " aliquot leases";
    return true;
  }

  // Rows that still name `uuid` (in_use): (table, column) pairs per table.
  Result<std::string> referrers(CatalogTable t, Uuid uuid) {
    std::vector<std::pair<const char*, const char*>> refs;
    switch (t) {
      case CatalogTable::PrincipalInvestigator: refs = {{"project", "pi_uuid"}}; break;
      case CatalogTable::Project: refs = {{"sample", "project_uuid"}}; break;
      case CatalogTable::Material: refs = {{"sample", "material_uuid"}}; break;
      case CatalogTable::Sample: refs = {{"irradiation_position", "sample_uuid"}, {"identifier", "sample_uuid"}}; break;
      case CatalogTable::Irradiation: refs = {{"level", "irradiation_uuid"}, {"ref_object", "irradiation_uuid"}}; break;
      case CatalogTable::Level: refs = {{"irradiation_position", "level_uuid"}, {"ref_object", "level_uuid"}}; break;
      case CatalogTable::IrradiationPosition: refs = {{"ref_object", "position_uuid"}}; break;
      default: break;
    }
    std::string out;
    for (const auto& [table, column] : refs) {
      auto n = count(QStringLiteral("SELECT count(*) FROM %1 WHERE %2 = ?").arg(QString::fromUtf8(table),
                                                                                 QString::fromUtf8(column)),
                     {qv(uuid)});
      if (!n) return fail(n.error());
      if (*n == 0) continue;
      if (!out.empty()) out += ", ";
      out += std::to_string(*n) + " " + table;
    }
    return out;
  }

  // ------------------------------------------------------------ edits

  Result<void> apply(const CatalogInsert& e) {
    auto t = table(e.table, "insert");
    if (!t) return fail(t.error());
    const TableRules& rules = **t;
    if (!rules.insert)
      return fail(ErrorKind::Protocol, std::string(table_name(e.table)) + " rows are not inserted by a catalog edit");
    if (e.uuid.is_nil()) return fail(ErrorKind::Protocol, "insert into " + std::string(table_name(e.table)) + " has no uuid");
    if (auto r = check_fields(rules, e.values); !r) return r;
    for (const auto& c : rules.columns)
      if (c.required && !e.values.count(c.name))
        return fail(ErrorKind::Protocol,
                    "insert into " + std::string(table_name(e.table)) + " needs " + c.name);

    CatalogFields row;
    for (const auto& c : rules.columns) {
      const auto it = e.values.find(c.name);
      if (it != e.values.end()) {
        row[c.name] = normalized(c, it->second);
      } else if (c.not_null) {
        row[c.name] = std::string();  // first_initial, grainsize: '' (the column default)
        if (e.table == CatalogTable::Irradiation && std::string(c.name) == "kind") row[c.name] = std::string("irradiation");
      }
    }
    auto clash = unique_clash(rules, e.uuid, row);
    if (!clash) return fail(clash.error());
    if (*clash) return {};

    Row insert;
    insert["uuid"] = qv(e.uuid);
    QJsonObject diff;
    for (const auto& [name, value] : row) {
      if (!is_geo(column_of(rules, name)->type)) insert[QString::fromStdString(name)] = to_variant(value);
      if (!std::holds_alternative<std::monostate>(value))
        diff.insert(QString::fromStdString(name), QJsonArray{QJsonValue(QJsonValue::Null), json_of(value)});
    }
    if (has_geo(rules)) {
      auto geom = geom_cell(rules, row);
      if (!geom) return fail(geom.error());
      insert[QString::fromUtf8(kGeomColumn)] = *geom;
    }
    const UtcTime now = UtcTime::now();
    insert["created_utc"] = qv(now);
    if (e.table == CatalogTable::Sample) insert["updated_utc"] = qv(now);
    if (auto r = db_.insert(qstr(table_name(e.table)), insert); !r) return statement_failed(e.table, e.uuid, r.error());
    record(e.table, e.uuid, QStringLiteral("insert"), diff);
    return {};
  }

  Result<void> apply(const CatalogUpdate& e) {
    auto t = table(e.table, "update");
    if (!t) return fail(t.error());
    const TableRules& rules = **t;
    if (!rules.update)
      return fail(ErrorKind::Protocol, std::string(table_name(e.table)) + " rows are not updated by a catalog edit");
    if (auto r = check_fields(rules, e.expected); !r) return r;
    if (auto r = check_fields(rules, e.values); !r) return r;
    auto now = current(rules, e.uuid);
    if (!now) return fail(now.error());
    if (!stale_check(rules, e.table, e.uuid, e.expected, *now)) return {};
    const CatalogFields& before = **now;

    // Only what changes is written and audited.
    CatalogFields changes;
    for (const auto& [name, value] : e.values) {
      const CatalogValue v = normalized(*column_of(rules, name), value);
      if (before.at(name) != v) changes[name] = v;
    }
    if (changes.empty()) return {};

    CatalogFields after = before;
    for (const auto& [name, value] : changes) after[name] = value;

    if (auto r = update_rules(rules, e, before, after, changes); !r) return r;
    const std::size_t refused = refused_.size();
    bool key_changed = false;
    for (const char* k : rules.key) key_changed = key_changed || changes.count(k);
    if (key_changed) {
      auto clash = unique_clash(rules, e.uuid, after);
      if (!clash) return fail(clash.error());
    }
    if (refused_.size() != refused) return {};

    QStringList sets;
    Bindings b;
    QJsonObject diff;
    bool geo_changed = false;
    for (const auto& [name, value] : changes) {
      if (is_geo(column_of(rules, name)->type)) {
        geo_changed = true;
      } else {
        sets << QStringLiteral("\"%1\" = ?").arg(QString::fromStdString(name));
        b << to_variant(value);
      }
      diff.insert(QString::fromStdString(name), QJsonArray{json_of(before.at(name)), json_of(value)});
    }
    if (geo_changed) {
      auto geom = geom_cell(rules, after);
      if (!geom) return fail(geom.error());
      sets << QStringLiteral("\"%1\" = ?").arg(QString::fromUtf8(kGeomColumn));
      b << *geom;
    }
    if (e.table == CatalogTable::Sample) {
      sets << QStringLiteral("updated_utc = ?");
      b << qv(UtcTime::now());
    }
    b << qv(e.uuid);
    auto n = db_.affecting(
        QStringLiteral("UPDATE %1 SET %2 WHERE uuid = ?").arg(qstr(table_name(e.table)), sets.join(QStringLiteral(", "))),
        b);
    if (!n) return statement_failed(e.table, e.uuid, n.error());
    if (auto r = rewrite_ref_keys(e, before, after); !r) return r;
    record(e.table, e.uuid, QStringLiteral("update"), diff);
    return {};
  }

  Result<void> apply(const CatalogDelete& e) {
    auto t = table(e.table, "delete");
    if (!t) return fail(t.error());
    const TableRules& rules = **t;
    if (!rules.remove)
      return fail(ErrorKind::Protocol, std::string(table_name(e.table)) + " rows are not deleted by a catalog edit");
    if (auto r = check_fields(rules, e.expected); !r) return r;
    auto now = current(rules, e.uuid);
    if (!now) return fail(now.error());
    if (!stale_check(rules, e.table, e.uuid, e.expected, *now)) return {};
    const CatalogFields& before = **now;

    if (e.table == CatalogTable::Identifier) {
      std::string why;
      auto used = identifier_in_use(e.uuid, &why);
      if (!used) return fail(used.error());
      if (*used) {
        refuse(e.table, e.uuid, "analyzed_identifier",
               "identifier " + describe(before.at("identifier")) + " has " + why);
        return {};
      }
    }
    if (e.table == CatalogTable::IrradiationPosition) {
      auto held = db_.select_one(sql::kPositionIdentifier, {qv(e.uuid)});
      if (!held) return fail(held.error());
      if (*held) {
        refuse(e.table, e.uuid, "position_has_identifier",
               "position " + describe(before.at("position")) + " holds identifier " +
                   to_std((**held).value("identifier")));
        return {};
      }
    }
    auto users = referrers(e.table, e.uuid);
    if (!users) return fail(users.error());
    if (!users->empty()) {
      refuse(e.table, e.uuid, "in_use", std::string(table_name(e.table)) + " " + e.uuid.str() + " is named by " + *users);
      return {};
    }
    auto n = db_.affecting(QStringLiteral("DELETE FROM %1 WHERE uuid = ?").arg(qstr(table_name(e.table))), {qv(e.uuid)});
    if (!n) return statement_failed(e.table, e.uuid, n.error());
    QJsonObject diff;
    for (const auto& [name, value] : before)
      if (!std::holds_alternative<std::monostate>(value))
        diff.insert(QString::fromStdString(name), QJsonArray{json_of(value), QJsonValue(QJsonValue::Null)});
    record(e.table, e.uuid, QStringLiteral("delete"), diff);
    return {};
  }

  struct Audit {
    CatalogTable table;
    Uuid uuid;
    QString op;
    QJsonObject diff;  // column -> [before, after]
  };

  void record(CatalogTable table, Uuid uuid, const QString& op, const QJsonObject& diff) {
    for (auto it = audit_.begin(); it != audit_.end(); ++it) {
      if (it->table != table || it->uuid != uuid) continue;
      if (it->op == QStringLiteral("insert") && op == QStringLiteral("delete")) {
        audit_.erase(it);  // made and removed in one batch: nothing to record
        return;
      }
      for (auto d = diff.begin(); d != diff.end(); ++d) {
        const auto held = it->diff.find(d.key());
        if (held == it->diff.end()) {
          it->diff.insert(d.key(), d.value());
        } else {
          QJsonArray pair = held.value().toArray();
          pair[1] = d.value().toArray().at(1);
          it->diff.insert(d.key(), pair);
        }
      }
      if (it->op != QStringLiteral("insert")) it->op = op;
      return;
    }
    audit_.push_back(Audit{table, uuid, op, diff});
  }

  // True when the row exists and holds every expected value; otherwise the
  // row is recorded as stale.
  bool stale_check(const TableRules& rules, CatalogTable t, Uuid uuid, const CatalogFields& expected,
                   const std::optional<CatalogFields>& now) {
    if (!now) {
      stale_.push_back(StaleRow{t, uuid, expected, {}});
      return false;
    }
    CatalogFields actual;
    bool differs = false;
    for (const auto& [name, value] : expected) {
      actual[name] = now->at(name);
      if (now->at(name) != normalized(*column_of(rules, name), value)) differs = true;
    }
    if (!differs) return true;
    stale_.push_back(StaleRow{t, uuid, expected, std::move(actual)});
    return false;
  }

  Result<void> update_rules(const TableRules& rules, const CatalogUpdate& e, const CatalogFields& before,
                            const CatalogFields& after, const CatalogFields& changes) {
    (void)rules;
    (void)after;
    if (e.table == CatalogTable::Identifier) {
      std::string why;
      auto used = identifier_in_use(e.uuid, &why);
      if (!used) return fail(used.error());
      if (*used)
        refuse(e.table, e.uuid, "analyzed_identifier",
               "identifier " + describe(before.at("identifier")) + " has " + why);
    }
    if (e.table == CatalogTable::IrradiationPosition) {
      auto held = db_.select_one(sql::kPositionIdentifier, {qv(e.uuid)});
      if (!held) return fail(held.error());
      if (*held) {
        const Uuid identifier = to_uuid((**held).value("uuid"));
        std::string why;
        auto used = identifier_in_use(identifier, &why);
        if (!used) return fail(used.error());
        if (*used && (changes.count("level_uuid") || changes.count("position")))
          refuse(e.table, e.uuid, "analyzed_identifier",
                 "position " + describe(before.at("position")) + " holds identifier " +
                     to_std((**held).value("identifier")) + ", which has " + why + "; it cannot move");
        if (changes.count("sample_uuid") && !batch_.allow_analyzed_sample_change) {
          auto n = count(QStringLiteral("SELECT count(*) FROM analysis WHERE identifier_uuid = ?"), {qv(identifier)});
          if (!n) return fail(n.error());
          if (*n > 0)
            refuse(e.table, e.uuid, "analyzed_sample_change",
                   "position " + describe(before.at("position")) + " (identifier " +
                       to_std((**held).value("identifier")) + ") has " + std::to_string(*n) +
                       " analyses; changing its sample changes theirs");
        }
      }
    }
    if ((e.table == CatalogTable::Irradiation || e.table == CatalogTable::Level) && changes.count("name")) {
      auto n = count(e.table == CatalogTable::Irradiation ? sql::kAnalysesInIrradiation : sql::kAnalysesInLevel,
                     {qv(e.uuid)});
      if (!n) return fail(n.error());
      if (*n > 0)
        refuse(e.table, e.uuid, "analyzed_rename",
               std::string(e.table == CatalogTable::Irradiation ? "package " : "level ") +
                   describe(before.at("name")) + " has " + std::to_string(*n) + " analyses; it cannot be renamed");
    }
    if (e.table == CatalogTable::Level && changes.count("irradiation_uuid"))
      refuse(e.table, e.uuid, "constraint", "a level cannot move to another package");
    return {};
  }

  // A rename of a package or a level renames the keys of the references
  // scoped to it (E11): "<old>" and "<old>/..." become "<new>" and "<new>/...".
  Result<void> rewrite_ref_keys(const CatalogUpdate& e, const CatalogFields& before, const CatalogFields& after) {
    if (e.table != CatalogTable::Irradiation && e.table != CatalogTable::Level) return {};
    if (before.at("name") == after.at("name")) return {};
    std::string old_prefix, new_prefix;
    QString scope;
    if (e.table == CatalogTable::Irradiation) {
      old_prefix = std::get<std::string>(before.at("name"));
      new_prefix = std::get<std::string>(after.at("name"));
      scope = QStringLiteral("irradiation_uuid");
    } else {
      auto irrad = db_.select_one(QStringLiteral("SELECT name FROM irradiation WHERE uuid = ?"),
                                  {to_variant(before.at("irradiation_uuid"))});
      if (!irrad) return fail(irrad.error());
      if (!*irrad) return {};
      const std::string package = to_std((**irrad).value("name"));
      old_prefix = package + "/" + std::get<std::string>(before.at("name"));
      new_prefix = package + "/" + std::get<std::string>(after.at("name"));
      scope = QStringLiteral("level_uuid");
    }
    // Lengths are in characters for both engines; a QString counts UTF-16
    // units, so count the code points of the UTF-8 text.
    const auto chars = [](const std::string& s) {
      return static_cast<int>(std::count_if(s.begin(), s.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
    };
    const int old_len = chars(old_prefix);
    auto n = db_.affecting(sql::kRewriteRefKeys.arg(scope),
                           {qv(new_prefix), old_len + 1, qv(e.uuid), qv(old_prefix), old_len + 1, qv(old_prefix + "/")});
    if (!n) return statement_failed(e.table, e.uuid, n.error());
    return {};
  }

  // A statement the checks did not foresee failed. A constraint violation is
  // a refusal; it also ends the transaction on PostgreSQL, so the run stops.
  Result<void> statement_failed(CatalogTable t, Uuid uuid, const Error& error) {
    if (error.code != kCodeConstraint) return fail(error);
    refuse(t, uuid, "constraint", error.what);
    fatal_ = true;
    return {};
  }

  Db& db_;
  const CatalogEditBatch& batch_;
  std::vector<StaleRow> stale_;
  std::vector<Refusal> refused_;
  std::vector<Audit> audit_;
  bool fatal_ = false;
};

}  // namespace

// ---------------------------------------------------------------- reads

Result<std::vector<PrincipalInvestigatorRow>> principal_investigators(Db& db) {
  auto rows = db.select(sql::kPrincipalInvestigators);
  if (!rows) return fail(rows.error());
  std::vector<PrincipalInvestigatorRow> out;
  for (const auto& r : *rows)
    out.push_back(PrincipalInvestigatorRow{to_uuid(r.value("uuid")), to_std(r.value("last_name")),
                                           to_std(r.value("first_initial")), to_std(r.value("display_name")),
                                           opt_str(r.value("affiliation")), opt_str(r.value("email"))});
  return out;
}

Result<std::vector<ProjectRow>> projects(Db& db, std::optional<Uuid> pi) {
  QString sql = sql::kProjects;
  Bindings b;
  if (pi) {
    sql += QStringLiteral(" WHERE p.pi_uuid = ?");
    b << qv(*pi);
  }
  sql += QStringLiteral(" ORDER BY p.name, pi.display_name, p.uuid");
  auto rows = db.select(sql, b);
  if (!rows) return fail(rows.error());
  std::vector<ProjectRow> out;
  for (const auto& r : *rows) {
    ProjectRow p;
    p.uuid = to_uuid(r.value("uuid"));
    p.name = to_std(r.value("name"));
    p.principal_investigator = opt_uuid(r.value("pi_uuid"));
    p.principal_investigator_name = to_std(r.value("pi_name"));
    p.checkin_date = opt_str(r.value("checkin_date"));
    if (p.checkin_date && p.checkin_date->size() > 10) p.checkin_date->resize(10);  // QDate's ISO text
    p.comment = opt_str(r.value("comment"));
    p.lab_contact = opt_str(r.value("lab_contact"));
    p.institution = opt_str(r.value("institution"));
    p.n_samples = r.value("n_samples").toInt();
    out.push_back(std::move(p));
  }
  return out;
}

Result<std::vector<MaterialRow>> materials(Db& db) {
  auto rows = db.select(sql::kMaterials);
  if (!rows) return fail(rows.error());
  std::vector<MaterialRow> out;
  for (const auto& r : *rows)
    out.push_back(MaterialRow{to_uuid(r.value("uuid")), to_std(r.value("name")), to_std(r.value("grainsize")),
                              r.value("n_samples").toInt()});
  return out;
}

Result<std::vector<SampleRow>> samples(Db& db, Dialect dialect, const SampleQuery& q) {
  QString sql = sql::kSamples.arg(sql::ts(dialect, QStringLiteral("s.updated_utc")),
                                  geom_read(dialect, QStringLiteral("s.geom")));
  Bindings b;
  if (!q.text.empty()) {
    QString pattern = qs(q.text).toLower();
    pattern.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    pattern.replace(QStringLiteral("%"), QStringLiteral("\\%"));
    pattern.replace(QStringLiteral("_"), QStringLiteral("\\_"));
    sql += QStringLiteral(" AND lower(s.name) LIKE ? ESCAPE '\\'");
    b << (QStringLiteral("%") + pattern + QStringLiteral("%"));
  }
  if (q.principal_investigator) {
    sql += QStringLiteral(" AND p.pi_uuid = ?");
    b << qv(*q.principal_investigator);
  }
  if (q.project) {
    sql += QStringLiteral(" AND s.project_uuid = ?");
    b << qv(*q.project);
  }
  if (q.material) {
    sql += QStringLiteral(" AND s.material_uuid = ?");
    b << qv(*q.material);
  }
  sql += QStringLiteral(" ORDER BY lower(s.name), s.name, p.name, m.name, s.uuid LIMIT ?");
  b << std::max(q.limit, 0);
  auto rows = db.select(sql, b);
  if (!rows) return fail(rows.error());
  std::vector<SampleRow> out;
  for (const auto& r : *rows) {
    SampleRow s;
    s.uuid = to_uuid(r.value("uuid"));
    s.name = to_std(r.value("name"));
    s.project = to_uuid(r.value("project_uuid"));
    s.material = to_uuid(r.value("material_uuid"));
    s.principal_investigator = opt_uuid(r.value("pi_uuid"));
    s.project_name = to_std(r.value("project_name"));
    s.principal_investigator_name = to_std(r.value("pi_name"));
    s.material_name = to_std(r.value("material_name"));
    s.grainsize = to_std(r.value("grainsize"));
    auto& f = s.fields;
    f.note = opt_str(r.value("note"));
    f.igsn = opt_str(r.value("igsn"));
    if (const QVariant geom = r.value("geom"); !geom.isNull())
      if (const auto point = parse_point(to_std(geom))) {
        f.lat = point->lat;
        f.lon = point->lon;
      }
    f.elevation = opt_double(r.value("elevation"));
    f.storage_location = opt_str(r.value("storage_location"));
    f.location = opt_str(r.value("location"));
    f.unit = opt_str(r.value("unit"));
    f.lithology = opt_str(r.value("lithology"));
    f.lithology_class = opt_str(r.value("lithology_class"));
    f.lithology_type = opt_str(r.value("lithology_type"));
    f.lithology_group = opt_str(r.value("lithology_group"));
    f.approximate_age = opt_double(r.value("approximate_age"));
    s.updated = to_time(r.value("updated"));
    s.n_positions = r.value("n_positions").toInt();
    s.n_analyses = r.value("n_analyses").toInt();
    out.push_back(std::move(s));
  }
  return out;
}

Result<std::vector<IrradiationRow>> irradiations(Db& db, Dialect dialect) {
  auto rows = db.select(sql::kIrradiations.arg(sql::ts(dialect, QStringLiteral("i.created_utc"))));
  if (!rows) return fail(rows.error());
  std::vector<IrradiationRow> out;
  for (const auto& r : *rows)
    out.push_back(IrradiationRow{to_uuid(r.value("uuid")), to_std(r.value("name")), to_std(r.value("kind")),
                                 to_time(r.value("created")), r.value("n_levels").toInt(),
                                 r.value("n_positions").toInt(), r.value("n_analyzed").toInt(),
                                 r.value("n_chronology").toInt() > 0});
  return out;
}

namespace {
LevelRow level_from(const Row& r) {
  return LevelRow{to_uuid(r.value("uuid")),       to_uuid(r.value("irradiation_uuid")), to_std(r.value("name")),
                  opt_uuid(r.value("holder_ref_uuid")), opt_str(r.value("holder_name")), opt_str(r.value("note"))};
}
}  // namespace

Result<std::vector<LevelRow>> levels(Db& db, Uuid irradiation) {
  auto rows = db.select(sql::kLevelSelect + QStringLiteral(" WHERE l.irradiation_uuid = ? ORDER BY l.name, l.uuid"),
                        {qv(irradiation)});
  if (!rows) return fail(rows.error());
  std::vector<LevelRow> out;
  for (const auto& r : *rows) out.push_back(level_from(r));
  return out;
}

Result<std::optional<LevelSheet>> level_sheet(Db& db, Uuid level) {
  auto row = db.select_one(sql::kLevelSelect + QStringLiteral(" WHERE l.uuid = ?"), {qv(level)});
  if (!row) return fail(row.error());
  if (!*row) return std::optional<LevelSheet>{};
  LevelSheet sheet;
  sheet.level = level_from(**row);
  sheet.irradiation_name = to_std((**row).value("irradiation_name"));
  sheet.irradiation_kind = to_std((**row).value("irradiation_kind"));

  auto rows = db.select(sql::kSheetPositions, {qv(level)});
  if (!rows) return fail(rows.error());
  for (const auto& r : *rows) {
    PositionRow p;
    p.uuid = to_uuid(r.value("uuid"));
    p.position = r.value("position").toInt();
    p.sample = opt_uuid(r.value("sample_uuid"));
    p.sample_name = to_std(r.value("sample_name"));
    p.project = to_std(r.value("project_name"));
    p.principal_investigator = to_std(r.value("pi_name"));
    p.material = to_std(r.value("material_name"));
    p.grainsize = to_std(r.value("grainsize"));
    p.weight = opt_double(r.value("weight"));
    p.packet = opt_str(r.value("packet"));
    p.note = opt_str(r.value("note"));
    p.identifier_uuid = opt_uuid(r.value("identifier_uuid"));
    p.identifier = opt_str(r.value("identifier"));
    p.n_analyses = r.value("n_analyses").toInt();
    p.in_load = r.value("n_loads").toInt() > 0;
    p.j = opt_double(r.value("j"));
    p.j_err = opt_double(r.value("j_err"));
    sheet.positions.push_back(std::move(p));
  }

  auto heads = db.select(sql::kLevelRefHeads, {qv(level)});
  if (!heads) return fail(heads.error());
  for (const auto& r : *heads) {
    RefHead head{to_uuid(r.value("uuid")), opt_uuid(r.value("revision_uuid"))};
    const std::string type = to_std(r.value("ref_type"));
    std::optional<RefPayload> value;
    if (head.revision) {
      auto payload = read_payload(db, *head.revision, Kind::RefValue);
      if (!payload) return fail(payload.error());
      value = std::get<RefPayload>(*payload);
    }
    if (type == "level_geometry") {
      sheet.geometry = head;
      if (value) sheet.z = std::get<LevelZValue>(*value);
    } else {
      sheet.production = head;
      if (value) sheet.production_value = std::get<LevelProductionValue>(*value);
    }
  }
  return std::optional<LevelSheet>{std::move(sheet)};
}

Result<std::vector<RefObjectRow>> ref_objects(Db& db, RefType type, std::optional<Uuid> irradiation) {
  QString sql = QStringLiteral(
      "SELECT o.uuid, o.key, o.irradiation_uuid, o.level_uuid, h.revision_uuid FROM ref_object o "
      "LEFT JOIN head h ON h.subject_uuid = o.uuid AND h.kind = 'value' WHERE o.ref_type = ?");
  Bindings b{qstr(to_string(type))};
  if (irradiation) {
    sql += QStringLiteral(" AND o.irradiation_uuid = ?");
    b << qv(*irradiation);
  }
  sql += QStringLiteral(" ORDER BY o.key, o.uuid");
  auto rows = db.select(sql, b);
  if (!rows) return fail(rows.error());
  std::vector<RefObjectRow> out;
  for (const auto& r : *rows)
    out.push_back(RefObjectRow{to_uuid(r.value("uuid")), type, to_std(r.value("key")), opt_uuid(r.value("irradiation_uuid")),
                               opt_uuid(r.value("level_uuid")), opt_uuid(r.value("revision_uuid"))});
  return out;
}

Result<std::optional<std::int64_t>> identifier_counter(Db& db, const std::string& scope) {
  auto row = db.select_one(sql::kIdentifierCounter, {qv(scope)});
  if (!row) return fail(row.error());
  if (!*row) return std::optional<std::int64_t>{};
  return std::optional<std::int64_t>{static_cast<std::int64_t>((**row).value("last_value").toLongLong())};
}

Result<std::int64_t> max_numeric_identifier(Db& db, Dialect dialect) {
  auto row = db.select_one(sql::max_numeric_identifier(dialect));
  if (!row) return fail(row.error());
  if (!*row) return std::int64_t{0};
  return static_cast<std::int64_t>((**row).value("n").toLongLong());
}

Result<std::optional<CatalogFields>> catalog_row(Db& db, CatalogTable table, Uuid uuid) {
  const TableRules* rules = rules_of(table);
  if (!rules) return fail(ErrorKind::Protocol, "catalog_row: " + std::string(table_name(table)) + " is not an entry table");
  auto row = db.select_one(QStringLiteral("SELECT %1 FROM %2 WHERE uuid = ?")
                               .arg(select_list(*rules, db.dialect()), qstr(table_name(table))),
                           {qv(uuid)});
  if (!row) return fail(row.error());
  if (!*row) return std::optional<CatalogFields>{};
  return std::optional<CatalogFields>{fields_of(*rules, **row)};
}

// ---------------------------------------------------------------- writes

Result<CatalogOutcome> apply_catalog_edits(Db& db, Uuid client, const CatalogEditBatch& batch,
                                           StagedRefs* refs) {
  if (batch.edits.empty() && !refs) return CatalogOutcome{CatalogApplied{0}};
  WriteTx tx(db);
  if (auto r = tx.begin(); !r) return fail(r.error());
  EditRun run(db, batch);
  if (auto r = run.run(); !r) return fail(r.error());
  if (!run.stale().empty()) return CatalogOutcome{std::move(run.stale())};
  if (!run.refused().empty()) return CatalogOutcome{std::move(run.refused())};

  std::vector<ChangeEntityRow> entities = run.entities();
  std::optional<Uuid> changeset;
  QString kind = QStringLiteral("catalog");
  if (refs) {
    auto written = refs->write(entities);
    if (!written) return fail(written.error());
    if (!written->empty()) {
      tx.rollback();
      return CatalogOutcome{std::move(*written)};
    }
    changeset = refs->changeset();
    kind = QStringLiteral("changeset");
    // A reference object inserted by the batch is also a subject of the
    // changeset: one change_entity row each.
    std::vector<ChangeEntityRow> unique;
    for (auto& e : entities)
      if (std::none_of(unique.begin(), unique.end(), [&](const ChangeEntityRow& u) {
            return u.entity_type == e.entity_type && u.entity == e.entity;
          }))
        unique.push_back(std::move(e));
    entities = std::move(unique);
  }
  if (entities.empty() && !changeset) return CatalogOutcome{CatalogApplied{0}};
  auto seq = take_change(db, kind, changeset, client, entities);
  if (!seq) return fail(seq.error());
  if (auto r = tx.commit(); !r) return fail(r.error());
  return CatalogOutcome{CatalogApplied{*seq}};
}

Result<AllocationOutcome> allocate_identifiers(Db& db, Dialect dialect, Uuid client,
                                               const IdentifierAllocation& allocation) {
  const auto k = static_cast<std::int64_t>(allocation.assignments.size());
  std::vector<std::int64_t> numbers;
  std::set<Uuid> positions;
  for (const auto& a : allocation.assignments) {
    numbers.push_back(a.number);
    if (!positions.insert(a.position).second)
      return fail(ErrorKind::Protocol, "allocate_identifiers: position " + a.position.str() + " assigned twice");
  }
  std::sort(numbers.begin(), numbers.end());
  for (std::int64_t i = 0; i < k; ++i)
    if (numbers[static_cast<std::size_t>(i)] != allocation.expected_last + 1 + i)
      return fail(ErrorKind::Protocol, "allocate_identifiers: the numbers must be " +
                                           std::to_string(allocation.expected_last + 1) + " .. " +
                                           std::to_string(allocation.expected_last + k) + ", one each");
  if (k == 0) return AllocationOutcome{CatalogApplied{0}};

  WriteTx tx(db);
  if (auto r = tx.begin(); !r) return fail(r.error());
  const QString scope = qstr(kIdentifierScope);

  auto seed = max_numeric_identifier(db, dialect);
  if (!seed) return fail(seed.error());
  if (auto r = db.affecting(sql::kSeedCounter, {scope, static_cast<qlonglong>(*seed)}); !r) return fail(r.error());
  auto locked = db.select_one(dialect == Dialect::PostgreSql ? sql::kLockCounter : sql::kIdentifierCounter, {scope});
  if (!locked) return fail(locked.error());
  if (!*locked) return fail(ErrorKind::Protocol, "identifier_counter row is missing");
  const std::int64_t last = (**locked).value("last_value").toLongLong();
  if (last != allocation.expected_last) return AllocationOutcome{AllocationStale{last}};

  std::vector<Refusal> refused;
  std::vector<ChangeEntityRow> entities;
  for (const auto& a : allocation.assignments) {
    const std::string text = std::to_string(a.number);
    auto taken = db.select_one(sql::kIdentifierByTextAny, {qv(text)});
    if (!taken) return fail(taken.error());
    if (*taken) {
      refused.push_back(Refusal{CatalogTable::Identifier, to_uuid((**taken).value("uuid")), "unique",
                                "identifier " + text + " exists"});
      continue;
    }
    auto position = db.select_one(QStringLiteral("SELECT uuid FROM irradiation_position WHERE uuid = ?"), {qv(a.position)});
    if (!position) return fail(position.error());
    if (!*position) {
      refused.push_back(Refusal{CatalogTable::IrradiationPosition, a.position, "constraint", "no such position"});
      continue;
    }
    auto held = db.select_one(sql::kPositionIdentifier, {qv(a.position)});
    if (!held) return fail(held.error());
    const std::optional<Uuid> current = *held ? std::optional<Uuid>{to_uuid((**held).value("uuid"))} : std::nullopt;
    if (current != a.replaces) {
      refused.push_back(Refusal{CatalogTable::IrradiationPosition, a.position, "stale_identifier",
                                current ? "the position holds identifier " + to_std((**held).value("identifier"))
                                        : std::string("the position holds no identifier")});
      continue;
    }
    if (current) {
      auto use = db.select_one(sql::kIdentifierUse, {qv(*current), qv(*current), qv(*current)});
      if (!use) return fail(use.error());
      if ((**use).value("n_analyses").toLongLong() + (**use).value("n_loads").toLongLong() +
              (**use).value("n_leases").toLongLong() >
          0) {
        refused.push_back(Refusal{CatalogTable::Identifier, *current, "analyzed_identifier",
                                  "identifier " + to_std((**held).value("identifier")) +
                                      " is analyzed, loaded or leased"});
        continue;
      }
      auto n = db.affecting(QStringLiteral("UPDATE identifier SET identifier = ? WHERE uuid = ?"), {qv(text), qv(*current)});
      if (!n) return fail(n.error());
      QJsonObject diff{{QStringLiteral("identifier"),
                        QJsonArray{QJsonValue((**held).value("identifier").toString()), QJsonValue(qs(text))}}};
      entities.push_back(ChangeEntityRow{QStringLiteral("identifier"), *current, QStringLiteral("update"), compact(diff)});
    } else {
      const Uuid id = Uuid::v7();
      Row row;
      row["uuid"] = qv(id);
      row["identifier"] = qv(text);
      row["kind"] = QStringLiteral("unknown");
      row["position_uuid"] = qv(a.position);
      row["created_utc"] = qv(UtcTime::now());
      if (auto r = db.insert(QStringLiteral("identifier"), row); !r) return fail(r.error());
      entities.push_back(ChangeEntityRow{QStringLiteral("identifier"), id, QStringLiteral("insert"),
                                         json_created({{"identifier", text}, {"kind", std::string("unknown")}})});
    }
  }
  if (!refused.empty()) return AllocationOutcome{std::move(refused)};
  if (auto r = db.affecting(sql::kSetCounter, {static_cast<qlonglong>(allocation.expected_last + k), scope}); !r)
    return fail(r.error());
  auto seq = take_change(db, QStringLiteral("catalog"), std::nullopt, client, entities);
  if (!seq) return fail(seq.error());
  if (auto r = tx.commit(); !r) return fail(r.error());
  return AllocationOutcome{CatalogApplied{*seq}};
}

}  // namespace detail
}  // namespace pychron::persistence
