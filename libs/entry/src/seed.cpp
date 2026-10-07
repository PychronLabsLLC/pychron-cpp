#include "pychron/entry/seed.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <optional>
#include <set>
#include <utility>

#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>

#include "pychron/entry/names.hpp"
#include "pychron/entry/package_edit.hpp"

namespace pychron::entry {
namespace ps = pychron::persistence;
namespace {

// The analysis types a run of a reference sample has (the names of
// experiment's AnalysisType, which entry does not link).
constexpr std::array<std::string_view, 8> kReferenceTypes{
    "blank_unknown", "blank_air", "blank_cocktail", "blank_extractionline", "background", "air", "cocktail", "detector_ic"};

std::string lower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::string in_quotes(std::string_view s) { return "'" + std::string(s) + "'"; }

std::optional<double> number(const toml::node& n) {
  if (!n.is_floating_point() && !n.is_integer()) return std::nullopt;
  const auto v = n.value<double>();
  if (!v || !std::isfinite(*v)) return std::nullopt;
  return v;
}

}  // namespace

Result<Seed> parse_seed(std::string_view text, std::string_view name) {
  const std::string file(name);
  auto bad = [&](const std::string& what) { return fail(ErrorKind::Config, file + ": " + what); };
  auto parsed = toml::parse(text, name);
  if (!parsed) return bad("syntax error: " + std::string(parsed.error().description()));
  const toml::table& root = parsed.table();
  for (const auto& [k, v] : root) {
    if (k.str() != "project" && k.str() != "samples" && k.str() != "reactors") return bad("unknown key " + in_quotes(k.str()));
  }

  Seed seed;
  const auto project = root["project"].value<std::string>();
  if (!project) return bad("'project' must be the name of the project the samples are in");
  if (!valid_project_name(*project))
    return bad("project " + in_quotes(*project) + ": a letter, then letters, digits, '-' or '_'");
  seed.project = *project;

  if (const auto* node = root.get("samples")) {
    const auto* rows = node->as_array();
    if (!rows) return bad("'samples' must be [[samples]] tables");
    std::set<std::string> seen;
    for (std::size_t i = 0; i < rows->size(); ++i) {
      const std::string where = "samples[" + std::to_string(i + 1) + "]";
      const auto* row = (*rows)[i].as_table();
      if (!row) return bad(where + " must be a table");
      SeedSample s;
      const std::array<std::pair<std::string_view, std::string*>, 4> fields{
          {{"identifier", &s.identifier}, {"analysis_type", &s.analysis_type}, {"sample", &s.sample}, {"material", &s.material}}};
      for (const auto& [k, v] : *row) {
        if (std::none_of(fields.begin(), fields.end(), [&](const auto& f) { return f.first == k.str(); }))
          return bad(where + ": unknown key " + in_quotes(k.str()));
      }
      for (const auto& [key, out] : fields) {
        const auto v = (*row)[key].value<std::string>();
        if (!v || v->empty()) return bad(where + ": '" + std::string(key) + "' must be a non-empty string");
        *out = *v;
      }
      if (std::find(kReferenceTypes.begin(), kReferenceTypes.end(), s.analysis_type) == kReferenceTypes.end())
        return bad(where + ": analysis type " + in_quotes(s.analysis_type) + " has no reference sample");
      if (!seen.insert(lower(s.identifier)).second) return bad(where + ": identifier " + in_quotes(s.identifier) + " is given twice");
      seed.samples.push_back(std::move(s));
    }
  }

  if (const auto* node = root.get("reactors")) {
    const auto* reactors = node->as_table();
    if (!reactors) return bad("'reactors' must be [reactors.<name>] tables");
    const auto& keys = production_keys();
    for (const auto& [reactor_key, reactor_node] : *reactors) {
      const std::string reactor(reactor_key.str());
      const auto* ratios = reactor_node.as_table();
      if (!ratios) return bad("reactors." + reactor + " must be a table of ratios");
      for (const auto& [k, v] : *ratios) {
        if (std::find(keys.begin(), keys.end(), k.str()) == keys.end())
          return bad("reactors." + reactor + ": unknown key " + in_quotes(k.str()));
      }
      ps::ProductionValue value;
      value.reactor = reactor;
      for (const auto& key : keys) {
        const auto* given = ratios->get(key);
        if (!given) continue;
        const auto* pair = given->as_array();
        const auto v = pair && pair->size() == 2 ? number((*pair)[0]) : std::nullopt;
        const auto e = pair && pair->size() == 2 ? number((*pair)[1]) : std::nullopt;
        if (!v || !e) return bad("reactors." + reactor + "." + key + " must be [value, error], two finite numbers");
        value.ratios.push_back(ps::ProductionRatio{key, *v, *e});
      }
      seed.reactors.emplace(reactor, std::move(value));
    }
  }
  return seed;
}

namespace {

using Json = nlohmann::ordered_json;

// The reactors document as it is stored: where it is, its head, and its text.
struct StoredReactors {
  std::optional<ps::Uuid> object, head;
  std::string text;      // "" when there is no document, or it has no value yet
  bool as_text = false;  // kept in content_text, not content_json
};

Result<StoredReactors> stored_reactors(ps::IStore& store) {
  StoredReactors out;
  auto object = store.find_catalog_row(ps::CatalogTable::RefObject, {std::string("document"), std::string("reactors.json")});
  if (!object) return fail(object.error());
  if (!*object) return out;
  out.object = **object;
  auto head = store.head(**object, ps::Kind::RefValue);
  if (!head) return fail(head.error());
  if (!*head) return out;
  out.head = **head;
  auto payload = store.load_payload(**head);
  if (!payload) return fail(payload.error());
  if (!*payload) return out;
  const auto* ref = std::get_if<ps::RefPayload>(&**payload);
  const auto* doc = ref ? std::get_if<ps::DocumentValue>(ref) : nullptr;
  if (!doc) return fail(ErrorKind::Config, "reactors.json in the database is not a document; it was left as it is");
  if (doc->content_json) {
    out.text = *doc->content_json;
  } else if (doc->content_text) {
    out.text = *doc->content_text;
    out.as_text = true;
  }
  return out;
}

std::string counted(int n, const char* one) { return std::to_string(n) + " " + one + (n == 1 ? "" : "s"); }

}  // namespace

Result<SeedReport> apply_seed(ps::IStore& store, const Seed& seed, const ps::Actor& actor, bool dry_run) {
  SeedReport report;
  const ps::Uuid client = actor.client;

  // The project: one of that name, the one without a principal investigator first.
  std::optional<ps::Uuid> project;
  {
    auto rows = store.projects(std::nullopt);
    if (!rows) return fail(rows.error());
    for (const auto& r : *rows) {
      if (r.name != seed.project) continue;
      if (!project || !r.principal_investigator) project = r.uuid;
      if (!r.principal_investigator) break;
    }
    if (project) {
      report.kept.push_back("project " + seed.project);
    } else {
      ++report.projects;
      if (!dry_run) {
        ps::ProjectSpec spec;
        spec.name = seed.project;
        auto made = store.add_project(client, spec);
        if (!made) return fail(made.error());
        project = *made;
      }
    }
  }

  std::map<std::string, std::optional<ps::Uuid>> materials;  // nullopt: would be made (dry run)
  for (const auto& s : seed.samples) {
    if (materials.count(s.material)) continue;
    auto found = store.find_catalog_row(ps::CatalogTable::Material, {s.material, std::string()});
    if (!found) return fail(found.error());
    if (*found) {
      report.kept.push_back("material " + s.material);
    } else {
      ++report.materials;
      if (!dry_run) {
        ps::MaterialSpec spec;
        spec.name = s.material;
        auto made = store.add_material(client, spec);
        if (!made) return fail(made.error());
        *found = *made;
      }
    }
    materials.emplace(s.material, *found);
  }

  for (const auto& s : seed.samples) {
    const auto material = materials.at(s.material);
    std::optional<ps::Uuid> sample;
    if (project && material) {
      auto found = store.find_catalog_row(ps::CatalogTable::Sample, {s.sample, *project, *material});
      if (!found) return fail(found.error());
      sample = *found;
    }
    if (sample) {
      report.kept.push_back("sample " + s.sample);
    } else {
      ++report.samples;
      if (!dry_run) {
        ps::SampleSpec spec;
        spec.name = s.sample;
        spec.project = *project;
        spec.material = *material;
        auto made = store.add_sample(client, spec);
        if (!made) return fail(made.error());
        sample = *made;
      }
    }

    auto identifier = store.find_identifier(s.identifier);
    if (!identifier) return fail(identifier.error());
    if (*identifier) {
      report.kept.push_back("identifier " + s.identifier);
    } else {
      ++report.identifiers;
      if (!dry_run) {
        ps::IdentifierSpec spec;
        spec.identifier = s.identifier;
        spec.kind = "special";
        spec.analysis_type = s.analysis_type;
        spec.sample = sample;
        if (auto made = store.add_identifier(client, spec); !made) return fail(made.error());
      }
    }
  }

  if (seed.reactors.empty()) return report;
  auto stored = stored_reactors(store);
  if (!stored) return fail(stored.error());
  Json doc = Json::object();
  if (!stored->text.empty()) {
    doc = Json::parse(stored->text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object())
      return fail(ErrorKind::Config, "reactors.json in the database is not a JSON object; it was left as it is");
  }
  int added = 0;
  for (const auto& [name, value] : seed.reactors) {
    if (doc.contains(name)) {
      report.kept.push_back("reactor " + name);
      continue;
    }
    Json ratios = Json::object();
    for (const auto& r : value.ratios) ratios[r.key] = Json::array({r.value, r.error});
    doc[name] = std::move(ratios);
    ++added;
  }
  report.reactors = added;
  if (added == 0 || dry_run) return report;

  ps::Uuid object;
  if (stored->object) {
    object = *stored->object;
  } else {
    ps::RefObjectSpec spec;
    spec.type = ps::RefType::Document;
    spec.key = "reactors.json";
    auto made = store.add_ref_object(client, spec);
    if (!made) return fail(made.error());
    object = *made;
  }
  auto uow = store.begin(actor);
  if (!uow) return fail(uow.error());
  ps::DocumentValue value;
  (stored->as_text ? value.content_text : value.content_json) = doc.dump();
  if (auto r = (*uow)->add_revision(object, ps::Kind::RefValue, ps::RevisionPayload{ps::RefPayload{value}}, stored->head); !r)
    return fail(r.error());
  if (auto r = (*uow)->commit(ps::ChangesetKind::Reference, "seed: reactors"); !r) return fail(r.error());
  return report;
}

std::string describe(const SeedReport& r) {
  if (!r.changed()) return "seed: nothing to add (" + std::to_string(r.kept.size()) + " already there)";
  std::string out;
  auto part = [&](int n, const char* one) {
    if (n == 0) return;
    out += (out.empty() ? "seeded " : ", ") + counted(n, one);
  };
  part(r.projects, "project");
  part(r.materials, "material");
  part(r.samples, "sample");
  part(r.identifiers, "identifier");
  part(r.reactors, "reactor");
  return out;
}

}  // namespace pychron::entry
