#include "pychron/entry/package_edit.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include <nlohmann/json.hpp>

#include "pychron/entry/names.hpp"

namespace pychron::entry {

namespace ps = persistence;
using Json = nlohmann::json;

namespace {

// Seconds from `a` to `b`.
std::optional<double> seconds_between(const ps::UtcTime& a, const ps::UtcTime& b) {
  return static_cast<double>(b.micros - a.micros) / 1e6;
}

Result<std::optional<ps::RefPayload>> head_value(ps::IStore& store, const std::optional<ps::Uuid>& head) {
  if (!head) return std::optional<ps::RefPayload>{};
  auto payload = store.load_payload(*head);
  if (!payload) return fail(payload.error());
  if (!*payload) return std::optional<ps::RefPayload>{};
  const auto* ref = std::get_if<ps::RefPayload>(&**payload);
  if (!ref) return fail(ErrorKind::Protocol, "a reference head that is not a reference value");
  return std::optional<ps::RefPayload>{*ref};
}

}  // namespace

const std::vector<std::string>& production_keys() {
  static const std::vector<std::string> keys = {"K4039",  "K3839", "K3739", "Ca3937", "Ca3837",
                                                "Ca3637", "Cl3638", "Ca_K", "Cl_K"};
  return keys;
}

std::vector<std::string> validate(const NewPackage& p, const std::vector<std::string>& existing) {
  std::vector<std::string> out;
  if (!valid_package_name(p.name)) out.push_back("the name is empty or has spaces");
  if (std::find(existing.begin(), existing.end(), p.name) != existing.end())
    out.push_back("a package named " + p.name + " exists");
  if (p.kind != "irradiation" && p.kind != "package") out.push_back("the kind is irradiation or package");
  std::set<std::string> names;
  for (const auto& l : p.levels) {
    if (trim(l.name).empty()) out.push_back("a level has no name");
    else if (!names.insert(l.name).second) out.push_back("level " + l.name + " is listed twice");
  }
  if (p.kind == "irradiation") {
    if (!p.reactor || p.reactor->empty()) out.push_back("an irradiation needs a reactor");
    if (p.doses.empty()) out.push_back("an irradiation needs at least one dose");
    for (std::size_t i = 0; i < p.doses.size(); ++i) {
      const auto& d = p.doses[i];
      const std::string at = "dose " + std::to_string(i + 1);
      if (!(d.power > 0) || !std::isfinite(d.power)) out.push_back(at + ": power must be above 0");
      const auto length = seconds_between(d.start, d.end);
      if (!length || *length <= 0) out.push_back(at + ": the end must be after the start");
      if (i > 0) {
        const auto gap = seconds_between(p.doses[i - 1].end, d.start);
        if (gap && *gap < 0) out.push_back(at + " starts before dose " + std::to_string(i) + " ends");
      }
    }
  }
  return out;
}

Result<CreatedPackage> create_package(ps::IStore& store, const ps::Actor& actor, const NewPackage& p) {
  auto existing = store.irradiations();
  if (!existing) return fail(existing.error());
  std::vector<std::string> names;
  for (const auto& r : *existing) names.push_back(r.name);
  if (auto problems = validate(p, names); !problems.empty()) {
    std::string what = "package " + p.name + ":";
    for (const auto& m : problems) what += " " + m + ";";
    return fail(ErrorKind::Config, what);
  }

  CreatedPackage out;
  out.package = ps::Uuid::v7();
  ps::CatalogEditBatch batch;
  batch.message = "new package " + p.name;
  batch.edits.push_back(ps::CatalogInsert{ps::CatalogTable::Irradiation, out.package, {{"name", p.name}, {"kind", p.kind}}});
  for (const auto& l : p.levels) {
    const ps::Uuid id = ps::Uuid::v7();
    out.levels.push_back(id);
    ps::CatalogFields values{{"irradiation_uuid", out.package}, {"name", l.name}};
    if (l.holder) values["holder_ref_uuid"] = *l.holder;
    if (l.note) values["note"] = *l.note;
    batch.edits.push_back(ps::CatalogInsert{ps::CatalogTable::Level, id, std::move(values)});
  }

  auto uow = store.begin(actor);
  if (!uow) return fail(uow.error());
  bool staged = false;
  const auto object = [&](const char* type, const std::string& key, std::optional<ps::Uuid> level) {
    const ps::Uuid id = ps::Uuid::v7();
    ps::CatalogFields values{{"ref_type", std::string(type)}, {"key", key}, {"irradiation_uuid", out.package}};
    if (level) values["level_uuid"] = *level;
    batch.edits.push_back(ps::CatalogInsert{ps::CatalogTable::RefObject, id, std::move(values)});
    return id;
  };
  const auto stage = [&](ps::Uuid subject, ps::RefPayload value) -> Result<void> {
    auto r = (*uow)->add_revision(subject, ps::Kind::RefValue, ps::RevisionPayload{std::move(value)}, std::nullopt);
    if (!r) return fail(r.error());
    staged = true;
    return {};
  };

  std::optional<ps::Uuid> production;
  if (p.kind == "irradiation") {
    ps::ChronologyValue chronology;
    for (std::size_t i = 0; i < p.doses.size(); ++i) {
      ps::Dose d = p.doses[i];
      d.ordinal = static_cast<int>(i);
      chronology.doses.push_back(d);
    }
    if (auto r = stage(object("chronology", p.name, std::nullopt), chronology); !r) return fail(r.error());
    ps::ProductionValue value = p.production.value_or(ps::ProductionValue{});
    value.reactor = p.reactor;
    production = object("production", p.name + "/" + *p.reactor, std::nullopt);
    if (auto r = stage(*production, value); !r) return fail(r.error());
  }
  for (std::size_t i = 0; i < p.levels.size(); ++i) {
    const NewLevel& l = p.levels[i];
    const std::string key = p.name + "/" + l.name;
    if (production)
      if (auto r = stage(object("level_production", key, out.levels[i]), ps::LevelProductionValue{*production, std::nullopt}); !r)
        return fail(r.error());
    if (l.z)
      if (auto r = stage(object("level_geometry", key, out.levels[i]), ps::LevelZValue{l.z}); !r) return fail(r.error());
  }

  Result<ps::CatalogOutcome> outcome = staged
      ? store.apply_catalog_edits(actor, batch, **uow, ps::ChangesetKind::Reference, "new package " + p.name)
      : store.apply_catalog_edits(actor.client, batch);
  if (!outcome) return fail(outcome.error());
  if (const auto* done = std::get_if<ps::CatalogApplied>(&*outcome)) {
    out.seq = done->seq;
    return out;
  }
  if (const auto* refused = std::get_if<std::vector<ps::Refusal>>(&*outcome))
    return fail(ErrorKind::Config, "package " + p.name + ": " + (refused->empty() ? std::string("refused") : refused->front().what));
  return fail(ErrorKind::Config, "package " + p.name + ": another client changed the catalog; try again");
}

double dose_hours(const std::vector<ps::Dose>& doses) {
  double seconds = 0;
  for (const auto& d : doses)
    if (auto s = seconds_between(d.start, d.end); s && *s > 0) seconds += *s;
  return seconds / 3600.0;
}

double estimated_j(const std::vector<ps::Dose>& doses, const EntrySettings& settings) {
  return dose_hours(doses) * settings.j_multiplier;
}

Result<std::map<std::string, ps::ProductionValue>> parse_reactors(std::string_view text) {
  Json j = Json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return fail(ErrorKind::Config, "reactors: not a JSON object");
  std::map<std::string, ps::ProductionValue> out;
  for (auto it = j.begin(); it != j.end(); ++it) {
    if (!it.value().is_object()) continue;
    ps::ProductionValue value;
    value.reactor = it.key();
    for (const auto& key : production_keys()) {
      const auto r = it.value().find(key);
      if (r == it.value().end() || !r->is_array() || r->size() != 2 || !(*r)[0].is_number() || !(*r)[1].is_number())
        continue;
      value.ratios.push_back(ps::ProductionRatio{key, (*r)[0].get<double>(), (*r)[1].get<double>()});
    }
    out.emplace(it.key(), std::move(value));
  }
  return out;
}

Result<std::map<std::string, ps::ProductionValue>> load_reactors(ps::IStore& store) {
  auto object = store.find_catalog_row(ps::CatalogTable::RefObject, {std::string("document"), std::string("reactors.json")});
  if (!object) return fail(object.error());
  if (!*object) return std::map<std::string, ps::ProductionValue>{};
  auto head = store.head(**object, ps::Kind::RefValue);
  if (!head) return fail(head.error());
  auto value = head_value(store, *head);
  if (!value) return fail(value.error());
  if (!*value) return std::map<std::string, ps::ProductionValue>{};
  const auto* doc = std::get_if<ps::DocumentValue>(&**value);
  if (!doc) return fail(ErrorKind::Protocol, "reactors.json is not a document");
  return parse_reactors(doc->content_json ? *doc->content_json : doc->content_text.value_or("{}"));
}

Result<std::vector<NamedProduction>> package_productions(ps::IStore& store, ps::Uuid package,
                                                         const std::string& package_name) {
  auto rows = store.ref_objects(ps::RefType::Production, package);
  if (!rows) return fail(rows.error());
  std::vector<NamedProduction> out;
  const std::string prefix = package_name + "/";
  for (const auto& r : *rows) {
    NamedProduction p{r.uuid, r.key.rfind(prefix, 0) == 0 ? r.key.substr(prefix.size()) : r.key, r.head, {}};
    auto value = head_value(store, r.head);
    if (!value) return fail(value.error());
    if (*value)
      if (const auto* v = std::get_if<ps::ProductionValue>(&**value)) p.value = *v;
    out.push_back(std::move(p));
  }
  return out;
}

Result<PackageChronology> package_chronology(ps::IStore& store, ps::Uuid package, const std::string& package_name) {
  (void)package_name;
  auto rows = store.ref_objects(ps::RefType::Chronology, package);
  if (!rows) return fail(rows.error());
  PackageChronology out;
  if (rows->empty()) return out;
  out.ref_object = rows->front().uuid;
  out.head = rows->front().head;
  auto value = head_value(store, out.head);
  if (!value) return fail(value.error());
  if (*value)
    if (const auto* v = std::get_if<ps::ChronologyValue>(&**value)) out.value = *v;
  return out;
}

}  // namespace pychron::entry
