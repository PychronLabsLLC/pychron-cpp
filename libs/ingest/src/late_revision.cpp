#include "late_revision.hpp"

#include <cstddef>
#include <exception>
#include <optional>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "pychron/core/sha256.hpp"
#include "pychron/ingest/ids.hpp"

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

const char* cause_text(Late::Cause cause) {
  switch (cause) {
    case Late::Cause::HeadNotOfThisSource: return "head_not_of_this_source";
    case Late::Cause::LaterRevisionStored: return "later_revision_stored";
    case Late::Cause::StoredCommitUnknown: return "stored_commit_unknown";
    case Late::Cause::No: break;
  }
  return "";
}

// An analysis provenance row of a source that only made the analysis a member
// (writer.cpp, stage_membership).
bool membership_only(const P::ProvenanceRow& row) {
  if (!row.detail_json) return false;
  const Json parsed = Json::parse(*row.detail_json, nullptr, false);
  if (!parsed.is_object()) return false;
  const auto flag = parsed.find("membership_only");
  return flag != parsed.end() && flag->is_boolean() && flag->get<bool>();
}

}  // namespace

void StoredChains::begin_run(P::IStore& store, ISourceAdapter& adapter, P::Uuid source, std::string url) {
  store_ = &store;
  adapter_ = &adapter;
  source_ = source;
  url_ = std::move(url);
  chains_.clear();
  roots_.clear();
}

Result<void> StoredChains::sent_root(P::Uuid revision, std::string_view commit) {
  auto order = adapter_->order_of(commit);
  if (!order) return fail(order.error());
  roots_.insert_or_assign(revision, Ours{*order, std::string(commit)});
  return {};
}

// The commit a stored revision comes from and its place in the walk, when the
// revision is this source's:
//   - a root this run sent (its provenance may not be stored yet);
//   - a revision with a provenance row of this source;
//   - a root without a file of its own (it has no provenance row) of an
//     analysis this source created: it is of the record's commit.
Result<std::optional<StoredChains::Ours>> StoredChains::ours(const P::RevisionInfo& revision) {
  if (const auto sent = roots_.find(revision.uuid); sent != roots_.end()) return std::optional<Ours>{sent->second};
  const auto placed = [&](const std::string& commit) -> Result<std::optional<Ours>> {
    auto order = adapter_->order_of(commit);
    if (!order) return fail(order.error());
    return std::optional<Ours>{Ours{*order, commit}};
  };
  auto rows = store_->provenance_for(revision.uuid);
  if (!rows) return fail(rows.error());
  for (const auto& row : *rows)
    if (row.entity_type == "revision" && row.source == source_) return placed(row.commit_sha);
  if (revision.parent || P::subject_type_of(revision.kind) != P::SubjectType::Analysis) return std::optional<Ours>{};
  auto of_subject = store_->provenance_for(revision.subject);
  if (!of_subject) return fail(of_subject.error());
  for (const auto& row : *of_subject)
    if (row.entity_type == "analysis" && row.source == source_ && !membership_only(row) &&
        revision.changeset.uuid == collection_changeset_id(url_, row.commit_sha, revision.subject))
      return placed(row.commit_sha);
  return std::optional<Ours>{};
}

Result<StoredChains::Chain> StoredChains::load(P::Uuid subject, P::Kind kind) {
  Chain chain;
  auto stored = store_->history(subject, kind);
  if (!stored) return fail(stored.error());
  if (stored->empty()) return chain;
  chain.empty = false;
  auto head = store_->head(subject, kind);
  if (!head) return fail(head.error());
  for (const auto& revision : *stored) {
    auto mine = ours(revision);
    if (!mine) return fail(mine.error());
    if (!*mine) continue;
    if (*head && **head == revision.uuid) chain.head_ours = true;
    if (!(*mine)->order) {
      chain.unknown_commit = (*mine)->commit;
    } else if (!chain.last || *(*mine)->order > *chain.last) {
      chain.last = (*mine)->order;
      chain.last_commit = (*mine)->commit;
    }
  }
  return chain;
}

Result<Late> StoredChains::late(P::Uuid subject, P::Kind kind, std::optional<std::int64_t> order) {
  auto found = chains_.find({subject, kind});
  if (found == chains_.end()) {
    auto chain = load(subject, kind);
    if (!chain) return fail(chain.error());
    found = chains_.emplace(std::make_pair(subject, kind), std::move(*chain)).first;
  }
  const Chain& chain = found->second;
  if (chain.empty) return Late{};
  if (!chain.head_ours) return Late{Late::Cause::HeadNotOfThisSource};
  // A commit the walk does not have cannot be placed: it counts as later.
  if (!chain.unknown_commit.empty()) return Late{Late::Cause::StoredCommitUnknown, chain.unknown_commit};
  if (order && chain.last && *chain.last > *order) return Late{Late::Cause::LaterRevisionStored, chain.last_commit};
  return Late{};
}

void StoredChains::written(P::Uuid subject, P::Kind kind, std::optional<std::int64_t> order,
                           std::string_view commit) {
  const auto found = chains_.find({subject, kind});
  if (found == chains_.end()) return;  // not asked about in this run: loaded from the store when it is
  Chain& chain = found->second;
  chain.empty = false;
  chain.head_ours = true;
  if (order && (!chain.last || *order >= *chain.last)) {
    chain.last = order;
    chain.last_commit = std::string(commit);
  }
}

std::string late_revision_detail(const RevisionItem& revision, const Late& late) {
  Json out = Json::object();
  out["reason"] = std::string(kLateRevisionNotApplied);
  out["late"] = true;
  out["cause"] = cause_text(late.cause);
  if (!late.behind.empty()) out["behind"] = late.behind;
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
