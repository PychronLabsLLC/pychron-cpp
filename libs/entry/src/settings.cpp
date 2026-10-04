#include "pychron/entry/settings.hpp"

#include <nlohmann/json.hpp>

namespace pychron::entry {

namespace ps = persistence;
using Json = nlohmann::json;

Result<EntrySettings> parse_settings(std::string_view text) {
  Json j = Json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return fail(ErrorKind::Config, "entry settings: not a JSON object");
  EntrySettings s;
  Json other = Json::object();
  for (auto it = j.begin(); it != j.end(); ++it) {
    const std::string& k = it.key();
    const Json& v = it.value();
    const auto bad = [&](const char* type) {
      return fail(ErrorKind::Config, "entry settings: '" + k + "' must be " + type);
    };
    if (k == "package_prefix" || k == "default_package_kind" || k == "monitor_sample" || k == "monitor_material" ||
        k == "irradiation_project_prefix" || k == "null_identifier_rows") {
      if (!v.is_string()) return bad("text");
      std::string value = v.get<std::string>();
      if (k == "package_prefix") s.package_prefix = value;
      if (k == "default_package_kind") {
        if (value != "irradiation" && value != "package") return bad("irradiation or package");
        s.default_package_kind = value;
      }
      if (k == "monitor_sample") s.monitor_sample = value;
      if (k == "monitor_material") s.monitor_material = value;
      if (k == "irradiation_project_prefix") s.irradiation_project_prefix = value;
      if (k == "null_identifier_rows") {
        if (value != "allow" && value != "packet") return bad("allow or packet");
        s.null_identifier_rows = value;
      }
    } else if (k == "pi_names_allowed") {
      if (!v.is_array()) return bad("a list of text");
      s.pi_names_allowed.clear();
      for (const auto& e : v) {
        if (!e.is_string()) return bad("a list of text");
        s.pi_names_allowed.push_back(e.get<std::string>());
      }
    } else if (k == "create_irradiation_project") {
      if (!v.is_boolean()) return bad("true or false");
      s.create_irradiation_project = v.get<bool>();
    } else if (k == "j_multiplier") {
      if (!v.is_number()) return bad("a number");
      s.j_multiplier = v.get<double>();
    } else {
      other[k] = v;
    }
  }
  s.other_json = other.dump();
  return s;
}

std::string to_json(const EntrySettings& s) {
  Json j = Json::parse(s.other_json, nullptr, false);
  if (j.is_discarded() || !j.is_object()) j = Json::object();
  j["package_prefix"] = s.package_prefix;
  j["default_package_kind"] = s.default_package_kind;
  j["pi_names_allowed"] = s.pi_names_allowed;
  j["monitor_sample"] = s.monitor_sample;
  j["monitor_material"] = s.monitor_material;
  j["irradiation_project_prefix"] = s.irradiation_project_prefix;
  j["create_irradiation_project"] = s.create_irradiation_project;
  j["j_multiplier"] = s.j_multiplier;
  j["null_identifier_rows"] = s.null_identifier_rows;
  return j.dump(2);
}

Result<LoadedSettings> load_settings(ps::IStore& store) {
  LoadedSettings out;
  auto object = store.find_catalog_row(ps::CatalogTable::RefObject, {std::string("document"), std::string(kSettingsKey)});
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
  if (!doc) return fail(ErrorKind::Protocol, "entry settings: the reference is not a document");
  const std::string text = doc->content_json ? *doc->content_json : doc->content_text.value_or("{}");
  auto parsed = parse_settings(text);
  if (!parsed) return fail(parsed.error());
  out.settings = std::move(*parsed);
  return out;
}

Result<ps::CommitOutcome> save_settings(ps::IStore& store, const ps::Actor& actor, const EntrySettings& settings,
                                        const LoadedSettings& loaded) {
  ps::Uuid object;
  if (loaded.ref_object) {
    object = *loaded.ref_object;
  } else {
    ps::RefObjectSpec spec;
    spec.type = ps::RefType::Document;
    spec.key = std::string(kSettingsKey);
    auto made = store.add_ref_object(actor.client, spec);
    if (!made) return fail(made.error());
    object = *made;
  }
  auto uow = store.begin(actor);
  if (!uow) return fail(uow.error());
  ps::DocumentValue doc;
  doc.content_json = to_json(settings);
  if (auto r = (*uow)->add_revision(object, ps::Kind::RefValue, ps::RevisionPayload{ps::RefPayload{doc}}, loaded.head); !r)
    return fail(r.error());
  return (*uow)->commit(ps::ChangesetKind::Reference, "entry settings");
}

}  // namespace pychron::entry
