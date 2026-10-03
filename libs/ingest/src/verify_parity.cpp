// Age parity: the ages the legacy system stored with each interpreted age
// against ages computed from the imported data as of that interpreted age
// (verify_parts.hpp, legacy ingestion spec sections 6.3 and 10.6).
//
// Where the legacy ages are. An interpreted-age revision keeps the whole
// legacy file in InterpretedAgeValue::doc_json. Its "analyses" list has one
// entry per member with "uuid", "record_id", "age" and "age_err"; the member
// rows of the payload carry no age, and lose the members whose analysis is
// not in the store, so the document is what is read.
//
// What is compared. The head revision of each interpreted age, as of the
// source commit that saved it (AsOf). A member whose analysis this source did
// not import is not comparable: the walk order of another source says nothing
// about this one's commits (spec 10.30).

#include <algorithm>
#include <cmath>
#include <exception>
#include <memory>
#include <utility>

#include <nlohmann/json.hpp>

#include "pychron/ingest/ids.hpp"
#include "verify_parts.hpp"

namespace pychron::ingest::detail {

namespace P = pychron::persistence;
using Json = nlohmann::json;
using P::Uuid;

namespace {

constexpr const char* kPending = "pending";
constexpr const char* kSuperseded = "superseded";

std::optional<double> number(const Json& object, const char* key) {
  const auto it = object.find(key);
  if (it == object.end() || !it->is_number()) return std::nullopt;
  return it->get<double>();
}

// |a - b| / max(|a|, |b|); 0 when both are the same value.
double relative_difference(double a, double b) {
  if (a == b) return 0.0;
  return std::abs(a - b) / std::max(std::abs(a), std::abs(b));
}

// Whether this source imported the analysis itself: it has the analysis's
// provenance row, and not one that only made it a member of a repository.
Result<bool> imported_here(const VerifySource& source, Uuid analysis) {
  auto rows = source.store.provenance_for(analysis);
  if (!rows) return fail(rows.error());
  for (const auto& row : *rows) {
    if (row.source != source.uuid || row.entity_type != "analysis") continue;
    const Json detail = Json::parse(row.detail_json.value_or("{}"), nullptr, false);
    const auto member = detail.is_object() ? detail.find("membership_only") : detail.end();
    const bool membership = detail.is_object() && member != detail.end() && member->is_boolean() && member->get<bool>();
    if (!membership) return true;
  }
  return false;
}

// The source commit of a revision this source imported; empty: it did not.
Result<std::string> commit_of(const VerifySource& source, Uuid revision) {
  auto rows = source.store.provenance_for(revision);
  if (!rows) return fail(rows.error());
  for (const auto& row : *rows)
    if (row.source == source.uuid && row.entity_type == "revision") return row.commit_sha;
  return std::string();
}

}  // namespace

Result<void> check_parity(const VerifySource& source, Uuid client, const std::set<std::string>& interpreted_ages,
                          const AgeFn& age_fn, const VerifyOptions& options, VerifyReport& report) {
  P::IStore& store = source.store;
  std::map<Uuid, P::ImportConflictRow> failed;  // by conflict id
  std::set<Uuid> passed;
  // By interpreted-age key: the subject and the members its head lists, for
  // the ages whose head could be read.
  std::map<std::string, std::pair<Uuid, std::set<Uuid>>> listed;
  const auto not_comparable = [&](const std::string& reason) {
    ++report.parity_not_comparable;
    ++report.not_comparable_reasons[reason.empty() ? std::string("not comparable") : reason];
  };

  for (const auto& key : interpreted_ages) {
    const Uuid subject = interpreted_age_id(source.url, key);
    auto history = store.history(subject, P::Kind::InterpretedAge);
    if (!history) return fail(history.error());
    if (history->empty()) continue;  // not imported: the accounting says so
    // One comparison per interpreted age: against its head revision.
    auto head = store.head(subject, P::Kind::InterpretedAge);
    if (!head) return fail(head.error());
    const auto at_head = std::find_if(history->begin(), history->end(),
                                      [&](const P::RevisionInfo& r) { return *head && r.uuid == **head; });
    if (at_head == history->end()) {
      not_comparable("interpreted age has no head");
      continue;
    }
    const P::RevisionInfo& latest = *at_head;
    auto payload = store.load_payload(latest.uuid);
    if (!payload) return fail(payload.error());
    const auto* value = *payload ? std::get_if<P::InterpretedAgeValue>(&**payload) : nullptr;
    if (!value) {
      not_comparable("interpreted age has no value");
      continue;
    }
    auto commit = commit_of(source, latest.uuid);
    if (!commit) return fail(commit.error());
    const AsOf as_of{subject, latest.uuid, latest.changeset.uuid, source.uuid, *commit, latest.changeset.created};

    try {
      const Json doc = Json::parse(value->doc_json, nullptr, false);
      const auto analyses = doc.is_object() ? doc.find("analyses") : doc.end();
      auto& members = listed[key];
      members.first = subject;
      if (!doc.is_object() || analyses == doc.end() || !analyses->is_array()) {
        not_comparable("interpreted age lists no analyses");
        continue;
      }
      for (const auto& member : *analyses) {
        if (!member.is_object()) {
          not_comparable("member has no uuid");
          continue;
        }
        const auto uuid_text = member.find("uuid");
        const auto analysis = uuid_text != member.end() && uuid_text->is_string()
                                  ? Uuid::parse(uuid_text->get<std::string>())
                                  : std::nullopt;
        if (!analysis) {
          not_comparable("member has no uuid");
          continue;
        }
        if (!members.second.insert(*analysis).second) continue;  // listed twice: one comparison
        const auto legacy_age = number(member, "age");
        const auto legacy_err = number(member, "age_err");
        if (!legacy_age) {
          not_comparable("no legacy age");
          continue;
        }
        auto heads = store.heads(*analysis);
        if (!heads) return fail(heads.error());
        if (heads->empty()) {
          not_comparable("analysis is not in the store");
          continue;
        }
        auto here = imported_here(source, *analysis);
        if (!here) return fail(here.error());
        if (!*here) {
          not_comparable("other_source");
          continue;
        }
        if (as_of.commit.empty()) {
          not_comparable("interpreted age revision is not an import of this source");
          continue;
        }
        if (!age_fn) {
          not_comparable("no age function");
          continue;
        }
        auto computed = age_fn(*analysis, as_of);
        if (!computed) return fail(computed.error());
        if (const auto* why = std::get_if<NotComparable>(&*computed)) {
          not_comparable(why->reason);
          continue;
        }
        const ComputedAge& age = std::get<ComputedAge>(*computed);
        const double age_difference = relative_difference(*legacy_age, age.age);
        const double err_difference = legacy_err ? relative_difference(*legacy_err, age.age_err) : 0.0;
        const Uuid conflict = conflict_id(source.url, "parity", analysis->str() + "/" + subject.str());
        // Written so that a NaN fails.
        if (age_difference <= options.tolerance && err_difference <= options.tolerance) {
          ++(legacy_err ? report.parity_pass : report.parity_pass_age_only);
          passed.insert(conflict);
          continue;
        }
        ++report.parity_fail;
        report.parity_failures.push_back({*analysis, subject, conflict, *legacy_age, age.age, legacy_err, age.age_err,
                                          age_difference, err_difference});
        Json detail{{"check", "age_parity"},
                    {"interpreted_age", subject.str()},
                    {"interpreted_age_revision", latest.uuid.str()},
                    {"as_of",
                     {{"changeset", as_of.changeset.str()}, {"commit", as_of.commit}, {"created", as_of.created.iso()}}},
                    {"legacy", {{"age", *legacy_age}}},
                    {"computed", {{"age", age.age}, {"age_err", age.age_err}}},
                    {"relative_difference", {{"age", age_difference}}},
                    {"tolerance", options.tolerance}};
        if (legacy_err) {
          detail["legacy"]["age_err"] = *legacy_err;
          detail["relative_difference"]["age_err"] = err_difference;
        }
        if (const auto record = member.find("record_id"); record != member.end() && record->is_string())
          detail["record_id"] = *record;
        failed.insert_or_assign(
            conflict, P::ImportConflictRow{conflict, key, *analysis, P::ConflictKind::ValueMismatch, latest.uuid,
                                           std::nullopt,
                                           detail.dump(-1, ' ', false, Json::error_handler_t::replace), kPending});
      }
    } catch (const std::exception& e) {
      return fail(ErrorKind::Protocol, "verify: interpreted age " + key + ": " + e.what());
    }
  }
  std::sort(report.parity_failures.begin(), report.parity_failures.end(),
            [](const ParityFailure& a, const ParityFailure& b) {
              return std::pair{a.interpreted_age, a.analysis} < std::pair{b.interpreted_age, b.analysis};
            });

  // The outcome: a conflict per failure; one whose comparison now passes is
  // superseded, and one that was superseded and fails again is pending again.
  // A stored row keeps its detail (the values of the run that first failed).
  // A pending one about a member the interpreted age no longer lists can
  // never be compared again: it is superseded too.
  if (listed.empty()) return {};
  auto stored = store.import_conflicts({source.uuid, P::ConflictKind::ValueMismatch, std::nullopt});
  if (!stored) return fail(stored.error());
  std::map<Uuid, std::string> resolutions;
  std::vector<Uuid> dropped;
  for (const auto& row : *stored) {
    resolutions.emplace(row.uuid, row.resolution);
    const auto age = listed.find(row.path);
    if (row.resolution != kPending || !row.entity || age == listed.end()) continue;
    const auto& [subject, members] = age->second;
    const bool of_this_age = row.uuid == conflict_id(source.url, "parity", row.entity->str() + "/" + subject.str());
    if (of_this_age && !members.contains(*row.entity)) dropped.push_back(row.uuid);
  }

  std::unique_ptr<P::IImportUnitOfWork> uow;
  const auto writing = [&]() -> Result<void> {
    if (uow) return {};
    auto begun = store.begin_import_batch(source.uuid, client);
    if (!begun) return fail(begun.error());
    uow = std::move(*begun);
    return {};
  };
  for (auto& [conflict, row] : failed) {
    const auto known = resolutions.find(conflict);
    if (known != resolutions.end() && known->second != kSuperseded) continue;  // pending, or resolved by hand
    if (auto r = writing(); !r) return r;
    if (known == resolutions.end()) {
      if (auto r = uow->add_conflict(std::move(row)); !r) return r;
    } else if (auto r = uow->resolve_conflict(conflict, kPending); !r) {
      return r;
    }
  }
  for (const Uuid conflict : passed) {
    const auto known = resolutions.find(conflict);
    if (known == resolutions.end() || known->second != kPending) continue;
    if (auto r = writing(); !r) return r;
    if (auto r = uow->resolve_conflict(conflict, kSuperseded); !r) return r;
  }
  for (const Uuid conflict : dropped) {
    if (auto r = writing(); !r) return r;
    if (auto r = uow->resolve_conflict(conflict, kSuperseded); !r) return r;
  }
  if (uow)
    if (auto seq = uow->commit(); !seq) return fail(seq.error());
  return {};
}

}  // namespace pychron::ingest::detail
