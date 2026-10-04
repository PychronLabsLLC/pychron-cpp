#include "late_revision.hpp"

#include <cstddef>
#include <exception>
#include <optional>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "pychron/core/sha256.hpp"

namespace pychron::ingest::detail {

namespace P = pychron::persistence;
using Json = nlohmann::json;

namespace {

constexpr std::size_t kMaxContentBytes = 64 * 1024;

template <class T>
void put(Json& out, const char* name, const std::optional<T>& value) {
  if (value) out[name] = *value;
}
void put(Json& out, const char* name, const std::optional<P::Uuid>& value) {
  if (value) out[name] = value->str();
}

// A member the model keeps as JSON text: as JSON, or as the text when it is not.
void put_text(Json& out, const char* name, const std::string& text) {
  Json parsed = Json::parse(text, nullptr, false);
  out[name] = parsed.is_discarded() ? Json(text) : std::move(parsed);
}
void put_text(Json& out, const char* name, const std::optional<std::string>& text) {
  if (text) put_text(out, name, *text);
}

Json manual(const P::ManualOverride& m) {
  Json out = Json::object();
  out["use_value"] = m.use_value;
  put(out, "value", m.value);
  out["use_error"] = m.use_error;
  put(out, "error", m.error);
  return out;
}

Json references(const std::vector<P::ReferenceRow>& rows) {
  Json out = Json::array();
  for (const auto& r : rows) {
    Json row = Json::object();
    row["ordinal"] = r.ordinal;
    put(row, "ref_analysis", r.ref_analysis);
    put(row, "record_id", r.record_id);
    row["exclude"] = r.exclude;
    out.push_back(std::move(row));
  }
  return out;
}

template <class Row, class Fn>
Json rows_of(const std::vector<Row>& rows, Fn&& one) {
  Json out = Json::array();
  for (const auto& r : rows) {
    Json row = Json::object();
    one(row, r);
    out.push_back(std::move(row));
  }
  return out;
}

Json json_of(const P::Intercepts& rows) {
  return rows_of(rows, [](Json& o, const P::InterceptRow& r) {
    o["isotope"] = r.isotope;
    o["detector"] = r.detector;
    put(o, "value", r.value);
    put(o, "error", r.error);
    put(o, "fit", r.fit);
    put(o, "error_type", r.error_type);
    put(o, "n", r.n);
    put(o, "fn", r.fn);
    put(o, "include_baseline_error", r.include_baseline_error);
    put_text(o, "filter_outliers", r.filter_outliers_json);
    put_text(o, "user_excluded", r.user_excluded_json);
    put_text(o, "outlier_excluded", r.outlier_excluded_json);
    o["reviewed"] = r.reviewed;
    o["manual"] = manual(r.manual);
    put_text(o, "extra", r.extra_json);
  });
}

Json json_of(const P::Baselines& rows) {
  return rows_of(rows, [](Json& o, const P::BaselineRow& r) {
    o["detector"] = r.detector;
    put(o, "value", r.value);
    put(o, "error", r.error);
    put(o, "fit", r.fit);
    put(o, "error_type", r.error_type);
    put(o, "n", r.n);
    put(o, "fn", r.fn);
    put_text(o, "filter_outliers", r.filter_outliers_json);
    put_text(o, "user_excluded", r.user_excluded_json);
    put(o, "modifier_value", r.modifier_value);
    put(o, "modifier_error", r.modifier_error);
    o["reviewed"] = r.reviewed;
    o["manual"] = manual(r.manual);
    put_text(o, "extra", r.extra_json);
  });
}

Json json_of(const P::Blanks& rows) {
  return rows_of(rows, [](Json& o, const P::BlankRow& r) {
    o["isotope"] = r.isotope;
    put(o, "value", r.value);
    put(o, "error", r.error);
    put(o, "fit", r.fit);
    put(o, "error_type", r.error_type);
    o["reviewed"] = r.reviewed;
    o["manual"] = manual(r.manual);
    put_text(o, "extra", r.extra_json);
    o["references"] = references(r.references);
  });
}

Json json_of(const P::IcFactors& rows) {
  return rows_of(rows, [](Json& o, const P::IcFactorRow& r) {
    o["detector"] = r.detector;
    put(o, "value", r.value);
    put(o, "error", r.error);
    put(o, "fit", r.fit);
    o["reviewed"] = r.reviewed;
    put(o, "reference_detector", r.reference_detector);
    put(o, "standard_ratio", r.standard_ratio);
    o["discrimination"] = r.discrimination;
    o["source_correction"] = r.source_correction;
    put_text(o, "reference_data", r.reference_data_json);
    o["manual"] = manual(r.manual);
    put_text(o, "extra", r.extra_json);
    o["references"] = references(r.references);
  });
}

Json json_of(const P::SignalRefs& rows) {
  return rows_of(rows, [](Json& o, const P::SignalRefRow& r) {
    o["series_kind"] = r.series_kind;
    o["series_key"] = r.series_key;
    o["detector"] = r.detector;
    o["blob_sha256"] = to_hex(r.blob_sha);
    put(o, "n_points", r.n_points);
    put(o, "start_index", r.start_index);
    put(o, "end_index", r.end_index);
  });
}

Json json_of(const P::TagValue& v) {
  Json out = Json::object();
  out["name"] = v.name;
  put(out, "note", v.note);
  put_text(out, "subgroup", v.subgroup_json);
  return out;
}

Json json_of(const P::AnnotationValue& v) {
  Json out = Json::object();
  put(out, "comment", v.comment);
  return out;
}

Json json_of(const P::RefPins& rows) {
  return rows_of(rows, [](Json& o, const P::RefPinRow& r) {
    o["ref_object"] = r.ref_object.str();
    o["ref_revision"] = r.ref_revision.str();
  });
}

Json json_of(const P::CosmogenicValue& v) {
  Json out = Json::object();
  put_text(out, "doc", v.doc_json);
  return out;
}

Json json_of(const P::IdentityValue& v) {
  Json out = Json::object();
  out["aliquot"] = v.aliquot;
  out["increment"] = v.increment;
  out["reason"] = v.reason;
  return out;
}

Json json_of(const P::InterpretedAgeValue& v) {
  Json out = Json::object();
  put(out, "age", v.age);
  put(out, "age_err", v.age_err);
  put(out, "age_kind", v.age_kind);
  put(out, "kca", v.kca);
  put(out, "kca_err", v.kca_err);
  put(out, "mswd", v.mswd);
  put(out, "nanalyses", v.nanalyses);
  put_text(out, "doc", v.doc_json);
  out["members"] = rows_of(v.members, [](Json& o, const P::InterpretedAgeMember& m) {
    o["analysis"] = m.analysis.str();
    put(o, "record_id", m.record_id);
    put(o, "plateau_step", m.plateau_step);
    put(o, "tag", m.tag);
  });
  return out;
}

Json json_of(const P::FluxValue& v) {
  Json out = Json::object();
  put(out, "j", v.j);
  put(out, "j_err", v.j_err);
  put(out, "mean_j", v.mean_j);
  put(out, "mean_j_err", v.mean_j_err);
  put(out, "mean_j_mswd", v.mean_j_mswd);
  put(out, "position_jerr", v.position_jerr);
  put(out, "lambda_k_total", v.lambda_k_total);
  put(out, "lambda_k_total_err", v.lambda_k_total_err);
  put(out, "monitor_name", v.monitor_name);
  put(out, "monitor_material", v.monitor_material);
  put(out, "monitor_age", v.monitor_age);
  put(out, "monitor_age_err", v.monitor_age_err);
  put_text(out, "options", v.options_json);
  put_text(out, "extra", v.extra_json);
  out["analyses"] = rows_of(v.analyses, [](Json& o, const P::FluxAnalysis& a) {
    put(o, "analysis", a.analysis);
    o["record_id"] = a.record_id;
    o["is_omitted"] = a.is_omitted;
  });
  return out;
}

Json json_of(const P::LevelZValue& v) {
  Json out = Json::object();
  put(out, "z", v.z);
  return out;
}

Json json_of(const P::ProductionValue& v) {
  Json out = Json::object();
  put(out, "reactor", v.reactor);
  put(out, "note", v.note);
  out["ratios"] = rows_of(v.ratios, [](Json& o, const P::ProductionRatio& r) {
    o["key"] = r.key;
    o["value"] = r.value;
    o["error"] = r.error;
  });
  return out;
}

Json json_of(const P::LevelProductionValue& v) {
  Json out = Json::object();
  put(out, "note", v.note);
  return out;
}

Json json_of(const P::ChronologyValue& v) {
  Json out = Json::object();
  out["doses"] = rows_of(v.doses, [](Json& o, const P::Dose& d) {
    o["ordinal"] = d.ordinal;
    o["power"] = d.power;
    o["start"] = d.start.iso();
    o["end"] = d.end.iso();
  });
  return out;
}

Json json_of(const P::GainsValue& v) {
  Json out = Json::object();
  out["gains"] = rows_of(v.gains, [](Json& o, const P::DetectorGain& g) {
    o["detector"] = g.detector;
    o["gain"] = g.gain;
  });
  return out;
}

Json json_of(const P::SensitivityValue& v) {
  Json out = Json::object();
  out["sensitivity"] = v.sensitivity;
  if (v.create_date) out["create_date"] = v.create_date->iso();
  put_text(out, "extra", v.extra_json);
  return out;
}

Json json_of(const P::HolderValue& v) {
  Json out = Json::object();
  put(out, "shape", v.shape);
  put(out, "radius", v.radius);
  out["has_hole_numbers"] = v.has_hole_numbers;
  out["holes"] = rows_of(v.holes, [](Json& o, const P::HolderHole& h) {
    o["ordinal"] = h.ordinal;
    o["hole_id"] = h.hole_id;
    o["x"] = h.x;
    o["y"] = h.y;
    put(o, "radius", h.radius);
  });
  return out;
}

Json json_of(const P::ScriptValue& v) {
  Json out = Json::object();
  out["body"] = v.body;
  return out;
}

Json json_of(const P::DocumentValue& v) {
  Json out = Json::object();
  put(out, "content_text", v.content_text);
  put_text(out, "content_json", v.content_json);
  return out;
}

Json json_of(const P::RefPayload& payload) {
  return std::visit([](const auto& value) { return json_of(value); }, payload);
}

// What the revision would have written. An identity and a level's production
// name their identifier and production as the batch does: the writer had not
// resolved them.
Json content_of(const RevisionItem& revision) {
  Json content = std::visit([](const auto& value) { return json_of(value); }, revision.payload);
  if (content.is_object()) {
    if (!revision.identifier.empty()) content["identifier"] = revision.identifier;
    if (!revision.production_key.empty()) content["production"] = revision.production_key;
  }
  return content;
}

std::string dump(const Json& json) { return json.dump(-1, ' ', false, Json::error_handler_t::replace); }

}  // namespace

Result<bool> ReplayOrder::behind_stored(P::IStore& store, P::Uuid source, P::Uuid subject, P::Kind kind) const {
  auto stored = store.history(subject, kind);
  if (!stored) return fail(stored.error());
  for (const auto& revision : *stored) {
    if (passed_.contains(revision.uuid)) continue;
    auto rows = store.provenance_for(revision.uuid);
    if (!rows) return fail(rows.error());
    for (const auto& row : *rows)
      if (row.entity_type == "revision" && row.source == source) return true;
  }
  return false;
}

std::string late_revision_detail(const RevisionItem& revision) {
  Json out = Json::object();
  out["reason"] = std::string(kLateRevisionNotApplied);
  out["commit"] = revision.key.commit;
  out["path"] = revision.key.path;
  out["kind"] = std::string(P::to_string(revision.kind));
  out["blob_sha"] = revision.key.blob_sha;
  try {
    Json content = content_of(revision);
    if (dump(content).size() <= kMaxContentBytes) out["content"] = std::move(content);
  } catch (const std::exception&) {
    // Text nlohmann cannot hold: the blob sha still names the content.
  }
  return dump(out);
}

bool is_late_revision_detail(std::string_view detail_json) {
  const Json parsed = Json::parse(detail_json, nullptr, false);
  if (!parsed.is_object()) return false;
  const auto reason = parsed.find("reason");
  return reason != parsed.end() && reason->is_string() &&
         reason->get_ref<const std::string&>() == kLateRevisionNotApplied;
}

}  // namespace pychron::ingest::detail
