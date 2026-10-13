#include "pychron/processing/flux_store.hpp"

#include <cmath>
#include <set>

#include <nlohmann/json.hpp>

namespace pychron::processing {

namespace ps = persistence;
using Json = nlohmann::json;

namespace {

Result<void> bad(const std::string& what) { return fail(ErrorKind::Config, "flux monitors: " + what); }

bool finite_positive(double v) { return std::isfinite(v) && v > 0; }

Result<void> validate(const MonitorSets& sets) {
  if (sets.sets.empty()) return bad("'monitors' must not be empty");
  std::set<std::string> names;
  for (const auto& s : sets.sets) {
    if (s.name.empty()) return bad("a set has no 'name'");
    if (!names.insert(s.name).second) return bad("'name' '" + s.name + "' is used twice");
    if (s.sample.empty()) return bad("'sample' of '" + s.name + "' must not be empty");
    if (!finite_positive(s.age_ma)) return bad("'age_ma' of '" + s.name + "' must be positive");
    if (!std::isfinite(s.age_err_ma) || s.age_err_ma < 0) return bad("'age_err_ma' of '" + s.name + "' must not be negative");
    const std::pair<const char*, const reduction::Measured*> lambdas[] = {{"lambda_ec", &s.lambda_ec},
                                                                          {"lambda_b", &s.lambda_b}};
    for (const auto& [key, m] : lambdas) {
      if (!finite_positive(m->value)) return bad(std::string("'") + key + "' of '" + s.name + "' must be positive");
      if (!std::isfinite(m->error) || m->error < 0)
        return bad(std::string("'") + key + "' of '" + s.name + "' has a negative sigma");
    }
  }
  if (!names.contains(sets.default_name)) return bad("'default' names no set: '" + sets.default_name + "'");
  return {};
}

// "'key' of 'set' ..." once the set's name is known, "'key' ..." for the name itself.
std::string of_set(const char* key, const std::string& set_name) {
  return std::string("flux monitors: '") + key + "'" + (set_name.empty() ? "" : " of '" + set_name + "'");
}

Result<std::string> text_of(const Json& set, const char* key, bool required, const std::string& set_name = {}) {
  auto it = set.find(key);
  if (it == set.end()) {
    if (required) return fail(ErrorKind::Config, of_set(key, set_name) + " is missing");
    return std::string();
  }
  if (!it->is_string()) return fail(ErrorKind::Config, of_set(key, set_name) + " must be text");
  return it->get<std::string>();
}

Result<double> number_of(const Json& set, const char* key, const std::string& set_name) {
  auto it = set.find(key);
  if (it == set.end() || !it->is_number())
    return fail(ErrorKind::Config, of_set(key, set_name) + " must be a number");
  return it->get<double>();
}

Result<reduction::Measured> pair_of(const Json& set, const char* key, const std::string& set_name) {
  auto it = set.find(key);
  if (it == set.end() || !it->is_array() || it->size() != 2 || !(*it)[0].is_number() || !(*it)[1].is_number())
    return fail(ErrorKind::Config, of_set(key, set_name) + " must be a [value, sigma] pair");
  return reduction::Measured{(*it)[0].get<double>(), (*it)[1].get<double>()};
}

Json measured_json(const reduction::Measured& m) { return Json::array({m.value, m.error}); }

MonitorSet fc2(std::string name, double age, double age_err, reduction::Measured ec, reduction::Measured b) {
  MonitorSet s;
  s.name = std::move(name);
  s.sample = "FC-2";
  s.material = "sanidine";
  s.age_ma = age;
  s.age_err_ma = age_err;
  s.lambda_ec = ec;
  s.lambda_b = b;
  return s;
}

}  // namespace

const MonitorSet* MonitorSets::find(std::string_view name) const {
  if (name.empty()) name = default_name;
  for (const auto& s : sets)
    if (s.name == name) return &s;
  return nullptr;
}

MonitorSets default_monitor_sets() {
  MonitorSets d;
  d.default_name = "FC-2 (Kuiper 2008)";
  d.sets = {fc2("FC-2 (Kuiper 2008)", 28.201, 0.046, {5.80e-11, 9.9e-13}, {4.883e-10, 1.4e-12}),
            fc2("FC-2 (Renne 1998)", 28.02, 0.16, {5.81e-11, 0.0}, {4.962e-10, 0.0})};
  return d;
}

Result<MonitorSets> parse_monitor_sets(std::string_view text) {
  Json j = Json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return fail(ErrorKind::Config, "flux monitors: not a JSON object");
  MonitorSets out;
  Json other = Json::object();
  for (auto it = j.begin(); it != j.end(); ++it) {
    const std::string& k = it.key();
    const Json& v = it.value();
    if (k == "default") {
      if (!v.is_string()) return fail(ErrorKind::Config, "flux monitors: 'default' must be text");
      out.default_name = v.get<std::string>();
    } else if (k == "monitors") {
      if (!v.is_array()) return fail(ErrorKind::Config, "flux monitors: 'monitors' must be a list");
      for (const auto& e : v) {
        if (!e.is_object()) return fail(ErrorKind::Config, "flux monitors: 'monitors' must be a list of objects");
        MonitorSet s;
        auto name = text_of(e, "name", true);
        if (!name) return fail(name.error());
        s.name = std::move(*name);
        auto sample = text_of(e, "sample", true, s.name);
        if (!sample) return fail(sample.error());
        s.sample = std::move(*sample);
        auto material = text_of(e, "material", false, s.name);
        if (!material) return fail(material.error());
        s.material = std::move(*material);
        auto age = number_of(e, "age_ma", s.name);
        if (!age) return fail(age.error());
        s.age_ma = *age;
        auto age_err = number_of(e, "age_err_ma", s.name);
        if (!age_err) return fail(age_err.error());
        s.age_err_ma = *age_err;
        auto ec = pair_of(e, "lambda_ec", s.name);
        if (!ec) return fail(ec.error());
        s.lambda_ec = *ec;
        auto b = pair_of(e, "lambda_b", s.name);
        if (!b) return fail(b.error());
        s.lambda_b = *b;
        out.sets.push_back(std::move(s));
      }
    } else {
      other[k] = v;
    }
  }
  if (!j.contains("monitors")) return fail(ErrorKind::Config, "flux monitors: no 'monitors'");
  if (!j.contains("default") && !out.sets.empty()) out.default_name = out.sets.front().name;
  out.other_json = other.dump();
  if (auto r = validate(out); !r) return fail(r.error());
  return out;
}

std::string to_json(const MonitorSets& sets) {
  Json j = Json::parse(sets.other_json, nullptr, false);
  if (j.is_discarded() || !j.is_object()) j = Json::object();
  j["default"] = sets.default_name;
  Json list = Json::array();
  for (const auto& s : sets.sets) {
    Json e = Json::object();
    e["name"] = s.name;
    e["sample"] = s.sample;
    e["material"] = s.material;
    e["age_ma"] = s.age_ma;
    e["age_err_ma"] = s.age_err_ma;
    e["lambda_ec"] = measured_json(s.lambda_ec);
    e["lambda_b"] = measured_json(s.lambda_b);
    list.push_back(std::move(e));
  }
  j["monitors"] = std::move(list);
  return j.dump(2);
}

Result<LoadedMonitorSets> load_monitor_sets(ps::IStore& store) {
  LoadedMonitorSets out;
  out.sets = default_monitor_sets();
  auto object =
      store.find_catalog_row(ps::CatalogTable::RefObject, {std::string("document"), std::string(kFluxMonitorsKey)});
  if (!object) return fail(object.error());
  if (!*object) return out;
  out.ref_object = **object;
  auto head = store.head(**object, ps::Kind::RefValue);
  if (!head) return fail(head.error());
  out.head = *head;
  if (!*head) return out;
  auto payload = store.load_payload(**head);
  if (!payload) return fail(payload.error());
  if (!*payload) return out;
  const auto* ref = std::get_if<ps::RefPayload>(&**payload);
  const auto* doc = ref ? std::get_if<ps::DocumentValue>(ref) : nullptr;
  if (!doc) return fail(ErrorKind::Protocol, "flux monitors: the reference is not a document");
  const std::string text = doc->content_json ? *doc->content_json : doc->content_text.value_or("{}");
  auto parsed = parse_monitor_sets(text);
  if (!parsed) return fail(parsed.error());
  out.sets = std::move(*parsed);
  return out;
}

Result<ps::CommitOutcome> save_monitor_sets(ps::IStore& store, const ps::Actor& actor, const MonitorSets& sets,
                                            const LoadedMonitorSets& loaded) {
  if (auto r = validate(sets); !r) return fail(r.error());
  ps::Uuid object;
  if (loaded.ref_object) {
    object = *loaded.ref_object;
  } else {
    ps::RefObjectSpec spec;
    spec.type = ps::RefType::Document;
    spec.key = std::string(kFluxMonitorsKey);
    auto made = store.add_ref_object(actor.client, spec);
    if (!made) return fail(made.error());
    object = *made;
  }
  auto uow = store.begin(actor);
  if (!uow) return fail(uow.error());
  ps::DocumentValue doc;
  doc.content_json = to_json(sets);
  if (auto r = (*uow)->add_revision(object, ps::Kind::RefValue, ps::RevisionPayload{ps::RefPayload{doc}}, loaded.head); !r)
    return fail(r.error());
  return (*uow)->commit(ps::ChangesetKind::Reference, "flux monitors");
}

}  // namespace pychron::processing
