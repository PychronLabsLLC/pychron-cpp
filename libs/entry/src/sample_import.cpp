#include "pychron/entry/sample_import.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

#include "pychron/entry/names.hpp"
#include "pychron/entry/sample_fields.hpp"
#include "pychron/core/number.hpp"

namespace pychron::entry {

namespace ps = persistence;

namespace {

struct FieldInfo {
  ImportField field;
  const char* name;
  std::vector<const char*> aliases;
};

const std::vector<FieldInfo>& infos() {
  static const std::vector<FieldInfo> all = {
      {ImportField::Sample, "sample", {"sample_name", "name"}},
      {ImportField::Project, "project", {"project_name"}},
      {ImportField::PrincipalInvestigator, "principal_investigator", {"pi", "principal investigator", "investigator"}},
      {ImportField::Material, "material", {"material_name", "mineral"}},
      {ImportField::Grainsize, "grainsize", {"grain_size", "grain size", "size"}},
      {ImportField::Note, "note", {"notes", "comment", "comments", "description"}},
      {ImportField::Igsn, "igsn", {}},
      {ImportField::Lat, "lat", {"latitude"}},
      {ImportField::Lon, "lon", {"long", "longitude", "lng"}},
      {ImportField::Elevation, "elevation", {"elev", "altitude"}},
      {ImportField::StorageLocation, "storage_location", {"storage", "storage location"}},
      {ImportField::Location, "location", {"locality", "site"}},
      {ImportField::Unit, "unit", {"geologic_unit", "formation"}},
      {ImportField::Lithology, "lithology", {"rock_type"}},
      {ImportField::LithologyClass, "lithology_class", {}},
      {ImportField::LithologyType, "lithology_type", {}},
      {ImportField::LithologyGroup, "lithology_group", {}},
      {ImportField::ApproximateAge, "approximate_age", {"age", "approx_age", "estimated_age"}},
      {ImportField::Easting, "easting", {"utm_easting", "utm_e"}},
      {ImportField::Northing, "northing", {"utm_northing", "utm_n"}},
      {ImportField::Zone, "zone", {"utm_zone"}},
  };
  return all;
}

std::string normalize_header(std::string_view h) {
  std::string out;
  for (char c : trim(h)) {
    const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    out.push_back(l == '-' ? '_' : l);
  }
  return out;
}

std::optional<double> parse_number(const std::string& text, bool* bad) {
  const std::string t = trim(text);
  if (t.empty()) return std::nullopt;
  const auto v = parse_double(t);
  if (!v) {
    *bad = true;
    return std::nullopt;
  }
  return v;
}

std::optional<std::string> opt(const std::string& text) {
  std::string t = trim(text);
  if (t.empty()) return std::nullopt;
  return t;
}

}  // namespace

const std::vector<ImportField>& import_fields() {
  static const std::vector<ImportField> all = [] {
    std::vector<ImportField> out;
    for (const auto& i : infos()) out.push_back(i.field);
    return out;
  }();
  return all;
}

std::string_view field_name(ImportField field) {
  for (const auto& i : infos())
    if (i.field == field) return i.name;
  return "";
}

std::optional<ImportField> field_for_header(std::string_view header) {
  const std::string h = normalize_header(header);
  for (const auto& i : infos()) {
    if (h == i.name) return i.field;
    for (const char* a : i.aliases)
      if (h == normalize_header(a)) return i.field;
  }
  return std::nullopt;
}

ColumnMapping default_mapping(const std::vector<std::string>& header) {
  ColumnMapping out;
  std::set<ImportField> used;
  for (const auto& h : header) {
    auto f = field_for_header(h);
    if (f && !used.insert(*f).second) f.reset();  // the first column of a field wins
    out.push_back(f);
  }
  return out;
}

Result<CatalogSnapshot> read_snapshot(ps::IStore& store) {
  CatalogSnapshot s;
  auto pis = store.principal_investigators();
  if (!pis) return fail(pis.error());
  s.principal_investigators = std::move(*pis);
  auto projects = store.projects(std::nullopt);
  if (!projects) return fail(projects.error());
  s.projects = std::move(*projects);
  auto materials = store.materials();
  if (!materials) return fail(materials.error());
  s.materials = std::move(*materials);
  ps::SampleQuery all;
  all.limit = 100'000'000;
  auto samples = store.samples(all);
  if (!samples) return fail(samples.error());
  s.samples = std::move(*samples);
  return s;
}

std::string_view to_string(RowState state) {
  switch (state) {
    case RowState::Create: return "create";
    case RowState::Exists: return "exists";
    case RowState::Update: return "update";
    case RowState::Error: return "error";
  }
  return "";
}

ps::CatalogFields sample_columns(const ps::SampleFields& f) {
  const auto text = [](const std::optional<std::string>& v) -> ps::CatalogValue {
    return v ? ps::CatalogValue{*v} : ps::CatalogValue{};
  };
  const auto num = [](const std::optional<double>& v) -> ps::CatalogValue {
    return v ? ps::CatalogValue{*v} : ps::CatalogValue{};
  };
  return {{"note", text(f.note)},
          {"igsn", text(f.igsn)},
          {"lat", num(f.lat)},
          {"lon", num(f.lon)},
          {"elevation", num(f.elevation)},
          {"storage_location", text(f.storage_location)},
          {"location", text(f.location)},
          {"unit", text(f.unit)},
          {"lithology", text(f.lithology)},
          {"lithology_class", text(f.lithology_class)},
          {"lithology_type", text(f.lithology_type)},
          {"lithology_group", text(f.lithology_group)},
          {"approximate_age", num(f.approximate_age)}};
}

ps::SampleFields sample_fields_of(const ps::CatalogFields& columns) {
  ps::SampleFields f;
  const auto text = [&](const char* name, std::optional<std::string>& out) {
    const auto it = columns.find(name);
    if (it != columns.end())
      if (const auto* s = std::get_if<std::string>(&it->second)) out = *s;
  };
  const auto num = [&](const char* name, std::optional<double>& out) {
    const auto it = columns.find(name);
    if (it == columns.end()) return;
    if (const auto* d = std::get_if<double>(&it->second)) out = *d;
    if (const auto* i = std::get_if<std::int64_t>(&it->second)) out = static_cast<double>(*i);
  };
  text("note", f.note);
  text("igsn", f.igsn);
  num("lat", f.lat);
  num("lon", f.lon);
  num("elevation", f.elevation);
  text("storage_location", f.storage_location);
  text("location", f.location);
  text("unit", f.unit);
  text("lithology", f.lithology);
  text("lithology_class", f.lithology_class);
  text("lithology_type", f.lithology_type);
  text("lithology_group", f.lithology_group);
  num("approximate_age", f.approximate_age);
  return f;
}

SampleImportPlan plan_sample_import(const CsvTable& table, const ColumnMapping& mapping, const CatalogSnapshot& catalog,
                                    const ImportOptions& options) {
  SampleImportPlan plan;
  std::set<std::string> new_pis, new_projects, new_materials;
  // (sample, project, PI, material, grainsize) -> line of first occurrence
  std::map<std::tuple<std::string, std::string, std::string, std::string, std::string>, int> seen;

  for (std::size_t r = 0; r < table.rows.size(); ++r) {
    const auto& cells = table.rows[r];
    ImportRow row;
    row.line = table.lines[r];
    std::map<ImportField, std::string> v;
    for (std::size_t c = 0; c < mapping.size() && c < cells.size(); ++c)
      if (mapping[c]) v[*mapping[c]] = cells[c];
    const auto get = [&](ImportField f) { return v.contains(f) ? trim(v[f]) : std::string(); };
    if (std::find(table.ragged.begin(), table.ragged.end(), row.line) != table.ragged.end())
      row.messages.emplace_back("the row does not have one value per column");

    row.sample = get(ImportField::Sample);
    row.project = get(ImportField::Project);
    row.material = get(ImportField::Material);
    row.grainsize = get(ImportField::Grainsize);
    const std::string pi_text = get(ImportField::PrincipalInvestigator);
    if (row.sample.empty()) row.messages.emplace_back("no sample name");
    if (row.project.empty()) row.messages.emplace_back("no project");
    if (row.material.empty()) row.messages.emplace_back("no material");
    if (pi_text.empty()) row.messages.emplace_back("no principal investigator");
    std::optional<PiName> pi;
    if (!pi_text.empty()) {
      auto parsed = parse_pi(pi_text, options.pi_names_allowed);
      if (parsed) {
        pi = *parsed;
        row.principal_investigator = display_name(*pi);
      } else {
        row.messages.push_back(parsed.error().what);
        row.principal_investigator = pi_text;
      }
    }
    if (!row.project.empty() && !valid_project_name(row.project))
      row.messages.push_back("project '" + row.project + "': start with a letter; use letters, digits, '-' or '_'");

    auto& f = row.fields;
    f.note = opt(get(ImportField::Note));
    f.igsn = opt(get(ImportField::Igsn));
    f.storage_location = opt(get(ImportField::StorageLocation));
    f.location = opt(get(ImportField::Location));
    f.unit = opt(get(ImportField::Unit));
    f.lithology = opt(get(ImportField::Lithology));
    f.lithology_class = opt(get(ImportField::LithologyClass));
    f.lithology_type = opt(get(ImportField::LithologyType));
    f.lithology_group = opt(get(ImportField::LithologyGroup));
    const auto number = [&](ImportField field) {
      bool bad = false;
      auto n = parse_number(get(field), &bad);
      if (bad) row.messages.push_back(std::string(field_name(field)) + " '" + get(field) + "' is not a number");
      return n;
    };
    f.lat = number(ImportField::Lat);
    f.lon = number(ImportField::Lon);
    f.elevation = number(ImportField::Elevation);
    f.approximate_age = number(ImportField::ApproximateAge);
    const auto easting = number(ImportField::Easting);
    const auto northing = number(ImportField::Northing);
    const std::string zone = get(ImportField::Zone);
    if (!f.lat && !f.lon && (easting || northing || !zone.empty())) {
      if (!easting || !northing || zone.empty()) {
        row.messages.emplace_back("UTM needs easting, northing and zone");
      } else {
        auto ll = utm_to_lat_lon(*easting, *northing, zone);
        if (ll) {
          f.lat = ll->lat;
          f.lon = ll->lon;
        } else {
          row.messages.push_back(ll.error().what);
        }
      }
    }
    if (auto ok = check_lat_lon(f.lat, f.lon); !ok) row.messages.push_back(ok.error().what);

    if (row.messages.empty()) {
      const auto key = std::make_tuple(row.sample, row.project, row.principal_investigator, row.material, row.grainsize);
      if (auto [it, fresh] = seen.emplace(key, row.line); !fresh)
        row.messages.push_back("the same sample as line " + std::to_string(it->second));
    }
    if (!row.messages.empty()) {
      row.state = RowState::Error;
      ++plan.errors;
      plan.rows.push_back(std::move(row));
      continue;
    }

    // Resolve against the catalog.
    const ps::PrincipalInvestigatorRow* pi_row = nullptr;
    for (const auto& p : catalog.principal_investigators)
      if (p.last_name == pi->last_name && p.first_initial == pi->first_initial) pi_row = &p;
    const ps::ProjectRow* project_row = nullptr;
    if (pi_row)
      for (const auto& p : catalog.projects)
        if (p.name == row.project && p.principal_investigator == pi_row->uuid) project_row = &p;
    const ps::MaterialRow* material_row = nullptr;
    for (const auto& m : catalog.materials)
      if (m.name == row.material && m.grainsize == row.grainsize) material_row = &m;
    const ps::SampleRow* sample_row = nullptr;
    if (project_row && material_row)
      for (const auto& s : catalog.samples)
        if (s.name == row.sample && s.project == project_row->uuid && s.material == material_row->uuid) sample_row = &s;

    if (!pi_row) new_pis.insert(row.principal_investigator);
    if (!project_row) new_projects.insert(row.project + " (" + row.principal_investigator + ")");
    if (!material_row)
      new_materials.insert(row.grainsize.empty() ? row.material : row.material + " (" + row.grainsize + ")");

    if (!sample_row) {
      row.state = RowState::Create;
      ++plan.creates;
    } else {
      row.existing = sample_row->uuid;
      const ps::CatalogFields given = sample_columns(row.fields);
      const ps::CatalogFields stored = sample_columns(sample_row->fields);
      for (const auto& [name, value] : given)
        if (!std::holds_alternative<std::monostate>(value) && stored.at(name) != value) row.changed.push_back(name);
      if (row.changed.empty()) {
        row.state = RowState::Exists;
        ++plan.exists;
      } else {
        row.state = RowState::Update;
        ++plan.updates;
        if (!options.update_existing) row.messages.emplace_back("differs from the stored sample; not updated");
      }
    }
    plan.rows.push_back(std::move(row));
  }
  plan.new_principal_investigators.assign(new_pis.begin(), new_pis.end());
  plan.new_projects.assign(new_projects.begin(), new_projects.end());
  plan.new_materials.assign(new_materials.begin(), new_materials.end());
  return plan;
}

ps::CatalogEditBatch to_batch(const SampleImportPlan& plan, const CatalogSnapshot& catalog,
                              const ImportOptions& options) {
  ps::CatalogEditBatch batch;
  batch.message = "sample import";
  std::vector<ps::CatalogEdit> pis, projects, materials, samples;
  std::map<std::pair<std::string, std::string>, ps::Uuid> pi_ids;
  std::map<std::pair<std::string, ps::Uuid>, ps::Uuid> project_ids;
  std::map<std::pair<std::string, std::string>, ps::Uuid> material_ids;
  for (const auto& p : catalog.principal_investigators) pi_ids[{p.last_name, p.first_initial}] = p.uuid;
  for (const auto& p : catalog.projects)
    if (p.principal_investigator) project_ids[{p.name, *p.principal_investigator}] = p.uuid;
  for (const auto& m : catalog.materials) material_ids[{m.name, m.grainsize}] = m.uuid;

  for (const auto& row : plan.rows) {
    if (row.state == RowState::Update && options.update_existing && row.existing) {
      const ps::SampleRow* stored = nullptr;
      for (const auto& s : catalog.samples)
        if (s.uuid == *row.existing) stored = &s;
      if (!stored) continue;
      const ps::CatalogFields given = sample_columns(row.fields);
      const ps::CatalogFields before = sample_columns(stored->fields);
      ps::CatalogUpdate u{ps::CatalogTable::Sample, *row.existing, {}, {}};
      for (const auto& name : row.changed) {
        u.expected[name] = before.at(name);
        u.values[name] = given.at(name);
      }
      samples.emplace_back(std::move(u));
      continue;
    }
    if (row.state != RowState::Create) continue;
    const PiName pi = *parse_pi(row.principal_investigator, options.pi_names_allowed);
    auto pi_it = pi_ids.find({pi.last_name, pi.first_initial});
    if (pi_it == pi_ids.end()) {
      const ps::Uuid id = ps::Uuid::v7();
      pi_it = pi_ids.emplace(std::make_pair(pi.last_name, pi.first_initial), id).first;
      pis.emplace_back(ps::CatalogInsert{ps::CatalogTable::PrincipalInvestigator, id,
                                      {{"last_name", pi.last_name}, {"first_initial", pi.first_initial}}});
    }
    auto project_it = project_ids.find({row.project, pi_it->second});
    if (project_it == project_ids.end()) {
      const ps::Uuid id = ps::Uuid::v7();
      project_it = project_ids.emplace(std::make_pair(row.project, pi_it->second), id).first;
      projects.emplace_back(ps::CatalogInsert{ps::CatalogTable::Project, id, {{"name", row.project}, {"pi_uuid", pi_it->second}}});
    }
    auto material_it = material_ids.find({row.material, row.grainsize});
    if (material_it == material_ids.end()) {
      const ps::Uuid id = ps::Uuid::v7();
      material_it = material_ids.emplace(std::make_pair(row.material, row.grainsize), id).first;
      materials.emplace_back(ps::CatalogInsert{ps::CatalogTable::Material, id, {{"name", row.material}, {"grainsize", row.grainsize}}});
    }
    ps::CatalogFields values = sample_columns(row.fields);
    for (auto it = values.begin(); it != values.end();)
      it = std::holds_alternative<std::monostate>(it->second) ? values.erase(it) : std::next(it);
    values["name"] = row.sample;
    values["project_uuid"] = project_it->second;
    values["material_uuid"] = material_it->second;
    samples.emplace_back(ps::CatalogInsert{ps::CatalogTable::Sample, ps::Uuid::v7(), std::move(values)});
  }
  for (auto* part : {&pis, &projects, &materials, &samples})
    for (auto& e : *part) batch.edits.push_back(std::move(e));
  return batch;
}

std::string template_csv() {
  std::vector<std::string> header;
  for (const auto f : import_fields()) header.emplace_back(field_name(f));
  return write_csv(header, {});
}

std::string errors_csv(const SampleImportPlan& plan) {
  std::vector<std::string> header{"line", "messages", "sample", "project", "principal_investigator", "material",
                                  "grainsize"};
  std::vector<std::vector<std::string>> rows;
  for (const auto& r : plan.rows) {
    if (r.state != RowState::Error) continue;
    std::string messages;
    for (const auto& m : r.messages) messages += (messages.empty() ? "" : "; ") + m;
    rows.push_back({std::to_string(r.line), messages, r.sample, r.project, r.principal_investigator, r.material,
                    r.grainsize});
  }
  return write_csv(header, rows);
}

}  // namespace pychron::entry
