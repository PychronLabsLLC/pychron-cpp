#include "sample_table_model.hpp"

#include <QBrush>
#include <QColor>
#include <QFont>

#include "theme.hpp"

namespace pychron::ui {

namespace ps = persistence;

namespace {

QString text(const ps::CatalogValue& v) {
  if (const auto* s = std::get_if<std::string>(&v)) return QString::fromStdString(*s);
  if (const auto* d = std::get_if<double>(&v)) return QString::number(*d, 'g', 10);
  if (const auto* i = std::get_if<std::int64_t>(&v)) return QString::number(*i);
  return {};
}

ps::CatalogValue opt_text(const std::optional<std::string>& v) { return v ? ps::CatalogValue{*v} : ps::CatalogValue{}; }
ps::CatalogValue opt_num(const std::optional<double>& v) { return v ? ps::CatalogValue{*v} : ps::CatalogValue{}; }

bool numeric(int column) {
  return column == SampleTableModel::Lat || column == SampleTableModel::Lon || column == SampleTableModel::Elevation;
}

}  // namespace

SampleTableModel::SampleTableModel(QObject* parent) : QAbstractTableModel(parent) {}

void SampleTableModel::set_rows(std::vector<ps::SampleRow> rows) {
  beginResetModel();
  rows_ = std::move(rows);
  new_.clear();
  edits_.clear();
  deletes_.clear();
  stale_.clear();
  endResetModel();
}

const ps::SampleRow* SampleTableModel::stored(int row) const {
  if (row < 0 || row >= stored_count()) return nullptr;
  return &rows_[static_cast<std::size_t>(row)];
}

void SampleTableModel::add_new(NewSample sample) {
  const int at = rowCount();
  beginInsertRows({}, at, at);
  new_.push_back(std::move(sample));
  endInsertRows();
}

bool SampleTableModel::mark_delete(int row) {
  const ps::SampleRow* r = stored(row);
  if (!r) {
    if (row >= stored_count() && row < rowCount()) {
      beginRemoveRows({}, row, row);
      new_.erase(new_.begin() + (row - stored_count()));
      endRemoveRows();
      return true;
    }
    return false;
  }
  if (r->n_positions > 0 || r->n_analyses > 0) return false;
  deletes_.insert(r->uuid);
  Q_EMIT dataChanged(index(row, 0), index(row, ColumnCount - 1));
  return true;
}

void SampleTableModel::revert() {
  beginResetModel();
  new_.clear();
  edits_.clear();
  deletes_.clear();
  endResetModel();
}

void SampleTableModel::mark_stale(const std::vector<ps::StaleRow>& stale) {
  for (const auto& s : stale)
    if (s.table == ps::CatalogTable::Sample) stale_[s.uuid] = s.actual;
  Q_EMIT dataChanged(index(0, 0), index(rowCount() - 1, ColumnCount - 1));
}

bool SampleTableModel::is_stale(int row) const {
  const ps::SampleRow* r = stored(row);
  return r && stale_.contains(r->uuid);
}

const char* SampleTableModel::column_name(int column) {
  switch (column) {
    case Name: return "name";
    case Lat: return "lat";
    case Lon: return "lon";
    case Elevation: return "elevation";
    case Unit: return "unit";
    case Lithology: return "lithology";
    case Location: return "location";
    case Storage: return "storage_location";
    case Igsn: return "igsn";
    case Note: return "note";
    default: return nullptr;
  }
}

ps::CatalogValue SampleTableModel::stored_value(const ps::SampleRow& r, int column) const {
  const auto& f = r.fields;
  switch (column) {
    case Name: return r.name;
    case Lat: return opt_num(f.lat);
    case Lon: return opt_num(f.lon);
    case Elevation: return opt_num(f.elevation);
    case Unit: return opt_text(f.unit);
    case Lithology: return opt_text(f.lithology);
    case Location: return opt_text(f.location);
    case Storage: return opt_text(f.storage_location);
    case Igsn: return opt_text(f.igsn);
    case Note: return opt_text(f.note);
    default: return {};
  }
}

ps::CatalogValue SampleTableModel::current_value(int row, int column) const {
  const ps::SampleRow* r = stored(row);
  if (!r) return {};
  if (const char* name = column_name(column)) {
    auto e = edits_.find(r->uuid);
    if (e != edits_.end())
      if (auto v = e->second.find(name); v != e->second.end()) return v->second;
  }
  return stored_value(*r, column);
}

int SampleTableModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(rows_.size() + new_.size());
}

int SampleTableModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : ColumnCount; }

QVariant SampleTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
  static const char* const names[] = {"Name",      "Project",  "PI",       "Material", "Grainsize", "Lat",
                                      "Lon",       "Elevation", "Unit",    "Lithology", "Location", "Storage",
                                      "IGSN",      "Note",     "Positions", "Analyses"};
  return section >= 0 && section < ColumnCount ? tr(names[section]) : QVariant();
}

QVariant SampleTableModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid()) return {};
  const int row = index.row(), column = index.column();
  if (is_new(row)) {
    const NewSample& n = new_[static_cast<std::size_t>(row - stored_count())];
    if (role == Qt::DisplayRole) {
      switch (column) {
        case Name: return QString::fromStdString(n.name);
        case Project: return QString::fromStdString(n.project);
        case PrincipalInvestigator: return QString::fromStdString(n.principal_investigator);
        case Material: return QString::fromStdString(n.material);
        case Grainsize: return QString::fromStdString(n.grainsize);
        case Lat: return n.fields.lat ? QVariant(*n.fields.lat) : QVariant();
        case Lon: return n.fields.lon ? QVariant(*n.fields.lon) : QVariant();
        case Note: return n.fields.note ? QString::fromStdString(*n.fields.note) : QVariant();
        case Positions: return tr("new");
        default: return {};
      }
    }
    if (role == Qt::BackgroundRole) return QBrush(theme().diff_added);
    return {};
  }
  const ps::SampleRow& r = rows_[static_cast<std::size_t>(row)];
  const bool deleted = deletes_.contains(r.uuid);
  if (role == Qt::DisplayRole || role == Qt::EditRole) {
    switch (column) {
      case Project: return QString::fromStdString(r.project_name);
      case PrincipalInvestigator: return QString::fromStdString(r.principal_investigator_name);
      case Material: return QString::fromStdString(r.material_name);
      case Grainsize: return QString::fromStdString(r.grainsize);
      case Positions: return r.n_positions;
      case Analyses: return r.n_analyses;
      default: return text(current_value(row, column));
    }
  }
  if (role == Qt::FontRole && deleted) {
    QFont f;
    f.setStrikeOut(true);
    return f;
  }
  if (role == Qt::BackgroundRole) {
    if (stale_.contains(r.uuid)) return QBrush(theme().warning_bg);
    if (deleted) return QBrush(theme().diff_removed);
    if (const char* name = column_name(column)) {
      auto e = edits_.find(r.uuid);
      if (e != edits_.end() && e->second.contains(name)) return QBrush(theme().diff_changed);
    }
    return {};
  }
  if (role == Qt::ToolTipRole && stale_.contains(r.uuid)) {
    QString tip = tr("Changed by another client since it was loaded:");
    for (const auto& [k, v] : stale_.at(r.uuid)) tip += QStringLiteral("\n%1 = %2").arg(QString::fromStdString(k), text(v));
    return tip;
  }
  return {};
}

Qt::ItemFlags SampleTableModel::flags(const QModelIndex& index) const {
  Qt::ItemFlags f = QAbstractTableModel::flags(index);
  if (index.isValid() && !is_new(index.row()) && column_name(index.column())) f |= Qt::ItemIsEditable;
  return f;
}

bool SampleTableModel::setData(const QModelIndex& index, const QVariant& value, int role) {
  if (role != Qt::EditRole || !index.isValid() || is_new(index.row())) return false;
  const char* name = column_name(index.column());
  if (!name) return false;
  const ps::SampleRow& r = rows_[static_cast<std::size_t>(index.row())];
  const QString t = value.toString().trimmed();
  ps::CatalogValue v;
  if (numeric(index.column())) {
    if (!t.isEmpty()) {
      bool ok = false;
      const double d = t.toDouble(&ok);
      if (!ok) return false;
      v = d;
    }
  } else if (!t.isEmpty()) {
    v = t.toStdString();
  }
  if (index.column() == Name && t.isEmpty()) return false;
  auto& e = edits_[r.uuid];
  if (v == stored_value(r, index.column())) {
    e.erase(name);
    if (e.empty()) edits_.erase(r.uuid);
  } else {
    e[name] = v;
  }
  Q_EMIT dataChanged(index, index);
  return true;
}

Result<ps::CatalogEditBatch> SampleTableModel::to_batch(const entry::CatalogSnapshot& catalog,
                                                        const std::vector<std::string>& pi_names_allowed) const {
  ps::CatalogEditBatch batch;
  batch.message = "samples";
  // Stored rows.
  for (const auto& r : rows_) {
    if (deletes_.contains(r.uuid)) {
      batch.edits.emplace_back(ps::CatalogDelete{ps::CatalogTable::Sample, r.uuid, {{"name", r.name}}});
      continue;
    }
    auto e = edits_.find(r.uuid);
    if (e == edits_.end()) continue;
    ps::CatalogUpdate u{ps::CatalogTable::Sample, r.uuid, {}, e->second};
    for (const auto& [k, v] : e->second) {
      for (int c = 0; c < ColumnCount; ++c)
        if (const char* n = column_name(c); n && k == n) u.expected[k] = stored_value(r, c);
    }
    const auto lat = u.values.contains("lat") ? u.values.at("lat") : opt_num(r.fields.lat);
    const auto lon = u.values.contains("lon") ? u.values.at("lon") : opt_num(r.fields.lon);
    const auto as_opt = [](const ps::CatalogValue& v) -> std::optional<double> {
      if (const auto* d = std::get_if<double>(&v)) return *d;
      return std::nullopt;
    };
    if (auto ok = entry::check_lat_lon(as_opt(lat), as_opt(lon)); !ok)
      return fail(ErrorKind::Config, r.name + ": " + ok.error().what);
    batch.edits.emplace_back(std::move(u));
  }
  // New samples, with what they name that does not exist yet.
  std::vector<ps::CatalogEdit> pis, projects, materials, samples;
  std::map<std::pair<std::string, std::string>, ps::Uuid> pi_ids, material_ids;
  std::map<std::pair<std::string, ps::Uuid>, ps::Uuid> project_ids;
  for (const auto& p : catalog.principal_investigators) pi_ids[{p.last_name, p.first_initial}] = p.uuid;
  for (const auto& p : catalog.projects)
    if (p.principal_investigator) project_ids[{p.name, *p.principal_investigator}] = p.uuid;
  for (const auto& m : catalog.materials) material_ids[{m.name, m.grainsize}] = m.uuid;
  for (const auto& n : new_) {
    auto pi = entry::parse_pi(n.principal_investigator, pi_names_allowed);
    if (!pi) return fail(ErrorKind::Config, n.name + ": " + pi.error().what);
    if (!entry::valid_project_name(n.project))
      return fail(ErrorKind::Config, n.name + ": project '" + n.project + "' starts with a letter; letters, digits, - or _");
    if (n.name.empty() || n.material.empty()) return fail(ErrorKind::Config, "a new sample needs a name and a material");
    if (auto ok = entry::check_lat_lon(n.fields.lat, n.fields.lon); !ok)
      return fail(ErrorKind::Config, n.name + ": " + ok.error().what);
    auto p = pi_ids.find({pi->last_name, pi->first_initial});
    if (p == pi_ids.end()) {
      const ps::Uuid id = ps::Uuid::v7();
      p = pi_ids.emplace(std::make_pair(pi->last_name, pi->first_initial), id).first;
      pis.emplace_back(ps::CatalogInsert{ps::CatalogTable::PrincipalInvestigator, id,
                                      {{"last_name", pi->last_name}, {"first_initial", pi->first_initial}}});
    }
    auto j = project_ids.find({n.project, p->second});
    if (j == project_ids.end()) {
      const ps::Uuid id = ps::Uuid::v7();
      j = project_ids.emplace(std::make_pair(n.project, p->second), id).first;
      projects.emplace_back(ps::CatalogInsert{ps::CatalogTable::Project, id, {{"name", n.project}, {"pi_uuid", p->second}}});
    }
    auto m = material_ids.find({n.material, n.grainsize});
    if (m == material_ids.end()) {
      const ps::Uuid id = ps::Uuid::v7();
      m = material_ids.emplace(std::make_pair(n.material, n.grainsize), id).first;
      materials.emplace_back(ps::CatalogInsert{ps::CatalogTable::Material, id, {{"name", n.material}, {"grainsize", n.grainsize}}});
    }
    ps::CatalogFields values = entry::sample_columns(n.fields);
    for (auto it = values.begin(); it != values.end();)
      it = std::holds_alternative<std::monostate>(it->second) ? values.erase(it) : std::next(it);
    values["name"] = n.name;
    values["project_uuid"] = j->second;
    values["material_uuid"] = m->second;
    samples.emplace_back(ps::CatalogInsert{ps::CatalogTable::Sample, ps::Uuid::v7(), std::move(values)});
  }
  for (auto* part : {&pis, &projects, &materials, &samples})
    for (auto& e : *part) batch.edits.push_back(std::move(e));
  return batch;
}

}  // namespace pychron::ui
