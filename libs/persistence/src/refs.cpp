// Reference resolution and the derived-value cache (DVC schema spec,
// sections 4.3, 6.2; invariant I14).

#include <algorithm>
#include <map>

#include "sql/statements.hpp"
#include "store_impl.hpp"

namespace pychron::persistence::detail {
namespace {

ResolvedRef candidate(const Row& r) {
  return ResolvedRef{to_uuid(r.value("uuid")), parse_ref_type(to_std(r.value("ref_type"))).value_or(RefType::Document),
                     to_std(r.value("key")), to_uuid(r.value("revision_uuid")), false};
}

}  // namespace

Result<RefResolution> resolve_refs(Db& db, Uuid analysis, const RefPolicy& policy) {
  auto scope = db.select_one(sql::kAnalysisScope, {qv(analysis)});
  if (!scope) return fail(scope.error());
  if (!*scope) return fail(ErrorKind::Protocol, "unknown analysis " + analysis.str());
  const Row& s = **scope;

  // Pins (refpins head): ref_object -> the revision the analysis is frozen to.
  std::map<Uuid, Uuid> pins;
  if (policy.honour_pins) {
    auto row = db.select_one(sql::kSelectHead, {qv(analysis), QStringLiteral("refpins")});
    if (!row) return fail(row.error());
    if (*row) {
      auto payload = read_payload(db, to_uuid((*row)->value("revision_uuid")), Kind::RefPins);
      if (!payload) return fail(payload.error());
      for (const auto& p : std::get<RefPins>(*payload)) pins[p.ref_object] = p.ref_revision;
    }
  }
  auto apply_pin = [&](ResolvedRef& ref) {
    if (auto it = pins.find(ref.ref_object); it != pins.end()) {
      ref.revision = it->second;
      ref.pinned = true;
    }
  };

  auto rows = db.select(sql::kRefCandidates, {s.value("position_uuid"), s.value("level_uuid"),
                                              s.value("irradiation_uuid"), s.value("mass_spectrometer_uuid")});
  if (!rows) return fail(rows.error());
  RefResolution out;
  for (const auto& r : *rows) {
    ResolvedRef ref = candidate(r);
    apply_pin(ref);
    out.refs.push_back(ref);
  }

  // The production a level uses is named by its level_production value at the
  // revision just resolved (pinned or head).
  std::vector<ResolvedRef> productions;
  for (const auto& ref : out.refs) {
    if (ref.type != RefType::LevelProduction) continue;
    auto payload = read_payload(db, ref.revision, Kind::RefValue);
    if (!payload) return fail(payload.error());
    const auto& lp = std::get<LevelProductionValue>(std::get<RefPayload>(*payload));
    auto prod = db.select_one(sql::kRefObjectAtHead, {qv(lp.production)});
    if (!prod) return fail(prod.error());
    if (!*prod) continue;  // named production has no value yet
    ResolvedRef p = candidate(**prod);
    apply_pin(p);
    productions.push_back(p);
  }
  out.refs.insert(out.refs.end(), productions.begin(), productions.end());
  std::sort(out.refs.begin(), out.refs.end(),
            [](const ResolvedRef& a, const ResolvedRef& b) { return a.ref_object < b.ref_object; });
  return out;
}

Result<RefResolution> resolve_refs_at(Db& db, Uuid analysis, Uuid bookmark) {
  auto out = resolve_refs(db, analysis, RefPolicy{false});
  if (!out) return fail(out.error());
  auto rows = db.select(sql::kBookmarkValueEntries, {qv(bookmark)});
  if (!rows) return fail(rows.error());
  std::map<Uuid, Uuid> pinned;
  for (const auto& r : *rows) pinned[to_uuid(r.value("subject_uuid"))] = to_uuid(r.value("revision_uuid"));
  for (auto& ref : out->refs) {
    if (auto it = pinned.find(ref.ref_object); it != pinned.end()) {
      ref.revision = it->second;
      ref.pinned = true;
    }
  }
  return out;
}

// Canonical text, one fact per line, hashed with SHA-256:
//   pychron-derived-fingerprint/1
//   head <kind> <revision>          for every head of the analysis, by kind name
//   ref <ref_object> <revision>     for every resolved reference, by ref_object
//   reduction_version <version>
Result<Sha256Digest> input_fingerprint(Db& db, Uuid analysis, const std::string& reduction_version) {
  auto heads = read_heads(db, analysis);
  if (!heads) return fail(heads.error());
  auto refs = resolve_refs(db, analysis, RefPolicy{});
  if (!refs) return fail(refs.error());
  return fingerprint_of(*heads, refs->refs, reduction_version);
}

Result<void> put_derived(Db& db, Uuid analysis, const Sha256Digest& fingerprint, const std::string& reduction_version,
                         const std::vector<DerivedRow>& rows) {
  const DerivedPut put{analysis, fingerprint, rows};
  return put_derived_many(db, std::span<const DerivedPut>(&put, 1), reduction_version);
}

Result<void> put_derived_many(Db& db, std::span<const DerivedPut> puts, const std::string& reduction_version) {
  // A cache, not a record: no revision and no change_log entry (section 4.3).
  WriteTx tx(db);
  if (auto r = tx.begin(); !r) return fail(r.error());
  const UtcTime now = UtcTime::now();
  for (const auto& put : puts) {
    // One fingerprint per analysis (age cache spec 4.2): what was computed
    // from other inputs goes before this is filed.
    if (auto gone = db.affecting(sql::kDeleteOtherDerived, {qv(put.analysis), qv(put.fingerprint)}); !gone)
      return fail(gone.error());
    for (const auto& d : put.rows) {
      Row r;
      r["analysis_uuid"] = qv(put.analysis);
      r["fingerprint"] = qv(put.fingerprint);
      r["name"] = qv(d.name);
      r["value"] = qv(d.value);
      r["error"] = qv(d.error);
      r["units"] = qv(d.units);
      r["reduction_version"] = qv(reduction_version);
      r["computed_utc"] = qv(now);
      if (auto ins = db.insert_or_ignore("derived_value", r); !ins) return fail(ins.error());
    }
  }
  return tx.commit();
}

Result<std::optional<std::vector<DerivedRow>>> get_derived(Db& db, Uuid analysis,
                                                          const std::string& reduction_version) {
  auto fp = input_fingerprint(db, analysis, reduction_version);
  if (!fp) return fail(fp.error());
  auto rows = db.select(sql::kDerivedRows, {qv(analysis), qv(*fp)});
  if (!rows) return fail(rows.error());
  if (rows->empty()) return std::optional<std::vector<DerivedRow>>{};
  std::vector<DerivedRow> out;
  for (const auto& r : *rows)
    out.push_back(DerivedRow{to_std(r.value("name")), opt_double(r.value("value")), opt_double(r.value("error")),
                             opt_str(r.value("units"))});
  return std::optional<std::vector<DerivedRow>>{std::move(out)};
}

Result<std::map<Uuid, Sha256Digest>> derived_fingerprints(Db& db, std::span<const Uuid> analyses) {
  // In parts: a statement takes a bounded number of values (999 on an older SQLite).
  constexpr std::size_t kPart = 500;
  std::map<Uuid, Sha256Digest> out;
  for (std::size_t at = 0; at < analyses.size(); at += kPart) {
    const auto part = analyses.subspan(at, std::min(kPart, analyses.size() - at));
    Bindings ids;
    for (const Uuid& id : part) ids << qv(id);
    auto rows = db.select(sql::derived_fingerprints(static_cast<qsizetype>(part.size())), ids);
    if (!rows) return fail(rows.error());
    for (const auto& r : *rows) out.insert_or_assign(to_uuid(r.value("analysis_uuid")), to_digest(r.value("fingerprint")));
  }
  return out;
}

Result<int> prune_derived(Db& db, Uuid analysis) {
  auto versions = db.select(sql::kDerivedVersions, {qv(analysis)});
  if (!versions) return fail(versions.error());
  int removed = 0;
  for (const auto& v : *versions) {
    const std::string version = to_std(v.value("reduction_version"));
    auto fp = input_fingerprint(db, analysis, version);
    if (!fp) return fail(fp.error());
    auto n = db.affecting(sql::kPruneDerived, {qv(analysis), qv(version), qv(*fp)});
    if (!n) return fail(n.error());
    removed += *n;
  }
  return removed;
}

}  // namespace pychron::persistence::detail

namespace pychron::persistence {

// The text is input_fingerprint's (detail, above).
Sha256Digest fingerprint_of(const std::vector<HeadInfo>& heads, const std::vector<ResolvedRef>& refs,
                            const std::string& reduction_version) {
  std::map<std::string, Uuid> by_kind;
  for (const auto& h : heads) by_kind[std::string(to_string(h.kind))] = h.revision;
  std::string text = "pychron-derived-fingerprint/1\n";
  for (const auto& [kind, rev] : by_kind) text += "head " + kind + " " + rev.str() + "\n";
  for (const auto& r : refs) text += "ref " + r.ref_object.str() + " " + r.revision.str() + "\n";
  text += "reduction_version " + reduction_version + "\n";
  return sha256(std::string_view{text});
}

}  // namespace pychron::persistence
