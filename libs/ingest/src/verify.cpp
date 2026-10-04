// `import verify`: accounting, idempotence, pending conflicts and age parity
// of one source (verify.hpp). The parts are in verify_parts.hpp.

#include "pychron/ingest/verify.hpp"

#include <algorithm>
#include <exception>
#include <set>
#include <tuple>
#include <utility>

#include <nlohmann/json.hpp>

#include "pychron/ingest/conflict_markers.hpp"
#include "pychron/ingest/ids.hpp"
#include "verify_parts.hpp"

namespace pychron::ingest {

namespace P = pychron::persistence;
using Json = nlohmann::json;

namespace {

// A conflict that only annotates what is in the store: a catalog row imported
// without an optional link, a catalog row made from repository contents (spec
// 10.26), a revision kept out of a chain it would have been written behind
// (spec 10.35; the head is right). Not one kept back behind a commit the walk
// no longer has: that history was rewritten.
bool is_annotation(const P::ImportConflictRow& row) { return is_warning_conflict(row); }

// The pending conflicts of the source: those that fail verify and those that
// do not. Resolved and superseded rows are not counted.
Result<void> count_pending(const detail::VerifySource& source, VerifyReport& report) {
  auto rows = source.store.import_conflicts({source.uuid, std::nullopt, std::string("pending")});
  if (!rows) return fail(rows.error());
  try {
    for (const auto& row : *rows) {
      auto& list = is_annotation(row) ? report.warning_conflicts : report.blocking_conflicts;
      list.push_back(row.uuid);
    }
  } catch (const std::exception& e) {
    return fail(ErrorKind::Protocol, std::string("verify: conflict detail: ") + e.what());
  }
  std::sort(report.blocking_conflicts.begin(), report.blocking_conflicts.end());
  std::sort(report.warning_conflicts.begin(), report.warning_conflicts.end());
  report.pending_blocking = static_cast<int>(report.blocking_conflicts.size());
  report.pending_warnings = static_cast<int>(report.warning_conflicts.size());
  return {};
}

}  // namespace

bool is_warning_conflict(const P::ImportConflictRow& row) {
  // Every marker is written on an identity_clash and nowhere else
  // (conflict_markers.hpp): the same word in the detail of another kind (a
  // key of a file that could not be read, say) does not make it a warning.
  if (row.kind != P::ConflictKind::IdentityClash) return false;
  const Json detail = Json::parse(row.detail_json, nullptr, false);
  if (!detail.is_object()) return false;
  const auto marked = [&](const char* key) {
    const auto flag = detail.find(key);
    return flag != detail.end() && flag->is_boolean() && flag->get<bool>();
  };
  if (marked(kMarkerImported) || marked(kMarkerSynthesized)) return true;
  if (!marked(kMarkerLate)) return false;
  const auto reason = detail.find(kDetailReason);
  return reason != detail.end() && reason->is_string() &&
         reason->get_ref<const std::string&>() == kReasonLateRevisionNotApplied;
}

Result<VerifyReport> verify(P::IStore& store, P::Uuid client, ISourceAdapter& adapter, const WriterConfig& config,
                            const AgeFn& age_fn, VerifyOptions options) {
  auto described = adapter.describe();
  if (!described) return fail(described.error());
  const std::string url = normalize_source_url(described->url);
  const detail::VerifySource source{store, source_id(described->kind, url, described->branch), url};

  VerifyReport report;
  report.source.current_head = described->head;
  {
    auto stored = store.import_sources();
    if (!stored) return fail(stored.error());
    for (const auto& info : *stored) {
      if (info.spec.uuid != source.uuid) continue;
      report.source.registered = true;
      report.source.status = info.status;
      report.source.done = info.done;
      report.source.total = info.total;
      report.source.stored_head = info.head_sha;
    }
  }
  WriterConfig dry = config;
  dry.dry_run = true;
  dry.replay = false;

  // Accounting. The adapter decides what each unit became as the import
  // does, so it is given the state an import would read.
  std::set<std::string> interpreted_ages;
  {
    BatchWriter reader(store, client, dry);
    if (auto opened = reader.open(adapter); !opened) return fail(opened.error());
    detail::Accountant accountant(source);
    auto walked = adapter.for_each_unit(reader.state(), [&](const SourceUnit& unit) -> Result<void> {
      if (!unit.interpreted_age.empty()) interpreted_ages.insert(unit.interpreted_age);
      return accountant.check(unit, report);
    });
    if (!walked) return fail(walked.error());
    std::sort(report.unaccounted.begin(), report.unaccounted.end(),
              [](const UnaccountedUnit& a, const UnaccountedUnit& b) {
                return std::tie(a.unit.path, a.unit.commit) < std::tie(b.unit.path, b.unit.commit);
              });
  }

  // Idempotence: what a run would add, resuming and walking it all again.
  for (const bool replay : {false, true}) {
    dry.replay = replay;
    BatchWriter writer(store, client, dry);
    auto stats = writer.run(adapter, std::nullopt, {}, {});
    if (!stats) return fail(stats.error());
    (replay ? report.replay_would_write : report.would_write) = stats->would_write;
  }

  if (auto r = detail::check_parity(source, client, interpreted_ages, age_fn, options, report); !r)
    return fail(r.error());
  if (auto r = count_pending(source, report); !r) return fail(r.error());
  return report;
}

}  // namespace pychron::ingest
