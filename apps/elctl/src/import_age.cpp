// The age function of `elctl import verify` (legacy ingestion spec, 10.6,
// 10.30 and 10.32): the age of an analysis computed from the imported data as
// it stood when an interpreted age was saved.
//
// "As it stood" is a place in the walk of the project repository, not a time
// (git author dates tie and run out of order). For each kind the reduction
// reads (intercepts, baselines, blanks, IC factors, tags) the revision used
// is the last one this source imported at or before the interpreted age's
// commit in that walk; a root revision without a provenance row of its own
// belongs to the commit of its analysis. The analysis is then built from
// those payloads by the same function the Data browser uses for the heads
// (processing::analysis_from_store) and reduced the same way
// (processing::reduce_analysis), so nothing of the reduction is repeated here.
//
// Reference data (flux, production, chronology, gains) comes from another
// source, whose walk cannot be compared with this one. The current reference
// heads are used when no reference object of the analysis has a revision
// made after the interpreted age's commit time; otherwise the analysis is
// not comparable.
//
// Whatever cannot be reproduced is NotComparable with a fixed reason, never
// a number made from defaults:
//   as_of_commit_not_in_walk        the interpreted age's commit is not on the branch
//   analysis_imported_after         the analysis entered the repository after it
//   identity_changed_after          the analysis was renumbered after it: its
//                                   irradiation position may be another one
//   revision_outside_source         a revision this source did not import lies
//                                   before the one that would be used
//   no_intercepts                   nothing to reduce at that point
//   reference_changed_after         spec 10.32
//   no_j, no_production, no_chronology
//   not_reducible                   the reduction refused its input
//   age_undefined                   no age came out (1 + J F <= 0, no F)

#include <cmath>
#include <memory>
#include <unordered_map>

#include "import_impl.hpp"
#include "pychron/dvc/git_reader.hpp"
#include "pychron/processing/reduced.hpp"
#include "pychron/processing/store_source.hpp"

namespace elctl::import_detail {

namespace dvc = pychron::dvc;
namespace ingest = pychron::ingest;
namespace processing = pychron::processing;

namespace {

using ingest::NotComparable;
using ingest::ParityAge;

// The kinds an analysis is built from; processing::analysis_from_store reads
// these and no other (annotation is a comment and has no part in the age).
constexpr P::Kind kReductionKinds[] = {P::Kind::Intercepts, P::Kind::Baselines, P::Kind::Blanks, P::Kind::IcFactors,
                                       P::Kind::Tags};

class AgeAsOf {
 public:
  AgeAsOf(P::IStore& store, P::Uuid source, std::unordered_map<std::string, std::size_t> walk)
      : store_(store), source_(source), walk_(std::move(walk)) {}

  Result<ParityAge> operator()(P::Uuid analysis, const ingest::AsOf& as_of) const {
    const auto no = [](const char* reason) { return ParityAge{NotComparable{reason}}; };
    if (as_of.source != source_) return no("other_source");
    const auto at = walk_.find(as_of.commit);
    if (at == walk_.end()) return no("as_of_commit_not_in_walk");
    const std::size_t limit = at->second;

    // Where the analysis itself entered the walk: the place of its root revisions.
    auto collected = place_of(analysis, "analysis");
    if (!collected) return fail(collected.error());
    if (!*collected || **collected > limit) return no("analysis_imported_after");

    auto renumbered = store_.history(analysis, P::Kind::Identity);
    if (!renumbered) return fail(renumbered.error());
    for (const auto& revision : *renumbered) {
      auto place = place_of(revision.uuid, "revision");
      if (!place) return fail(place.error());
      if (!*place || **place > limit) return no("identity_changed_after");
    }

    processing::StoreAnalysisParts parts;
    auto detail = store_.load_analysis_detail(analysis);
    if (!detail) return fail(detail.error());
    if (!*detail) return no("analysis is not in the store");
    parts.detail = std::move(**detail);

    for (const P::Kind kind : kReductionKinds) {
      auto history = store_.history(analysis, kind);
      if (!history) return fail(history.error());
      // The last revision at or before the interpreted age in walk order;
      // among those of one commit, the later in the store's order.
      std::optional<P::Uuid> chosen;
      std::size_t chosen_place = 0;
      bool unplaced_before = false, unplaced = false;
      for (const auto& revision : *history) {
        auto place = place_of(revision.uuid, "revision");
        if (!place) return fail(place.error());
        std::optional<std::size_t> where = *place;
        if (!where && !revision.parent) where = *collected;  // a root made with the analysis
        if (!where) {
          unplaced = true;  // an edit made in the store, or by another source
          continue;
        }
        if (*where > limit || (chosen && *where < chosen_place)) continue;
        chosen = revision.uuid;
        chosen_place = *where;
        unplaced_before = unplaced;
      }
      if (unplaced_before) return no("revision_outside_source");
      if (!chosen) continue;  // the analysis had none of this kind then
      auto payload = store_.load_payload(*chosen);
      if (!payload) return fail(payload.error());
      if (!*payload) continue;
      parts.heads.insert_or_assign(kind, std::move(**payload));
      parts.head_revisions.insert_or_assign(kind, *chosen);
    }
    if (!parts.heads.contains(P::Kind::Intercepts)) return no("no_intercepts");

    auto refs = store_.resolve_refs(analysis, P::RefPolicy{});
    if (!refs) return fail(refs.error());
    for (const auto& ref : refs->refs) {
      auto history = store_.history(ref.ref_object, P::Kind::RefValue);
      if (!history) return fail(history.error());
      for (const auto& revision : *history)
        if (revision.changeset.created > as_of.created) return no("reference_changed_after");
      auto payload = store_.load_payload(ref.revision);
      if (!payload) return fail(payload.error());
      if (*payload)
        if (const auto* value = std::get_if<P::RefPayload>(&**payload)) parts.refs.push_back(*value);
    }

    auto built = processing::analysis_from_store(parts);
    if (!built) return no("not_reducible");
    if (!built->context.flux) return no("no_j");
    if (!built->context.production) return no("no_production");
    if (built->context.chronology.empty()) return no("no_chronology");
    const auto reduced =
        processing::reduce_analysis(std::make_shared<const processing::Analysis>(std::move(*built)), {});
    if (!reduced || !reduced->arar) return no("not_reducible");
    if (!reduced->arar->ages) return no("age_undefined");
    // The analytical error (no J error): what the legacy file stores as age_err.
    const auto& age = reduced->arar->ages->age;
    if (!std::isfinite(age.nominal()) || !std::isfinite(age.std_dev())) return no("age_undefined");
    return ParityAge{ingest::ComputedAge{age.nominal(), age.std_dev()}};
  }

 private:
  // The place in the walk of the commit this source imported `entity` at;
  // nullopt: it has no provenance row of this source, or the commit is not
  // on the branch.
  Result<std::optional<std::size_t>> place_of(P::Uuid entity, std::string_view entity_type) const {
    auto rows = store_.provenance_for(entity);
    if (!rows) return fail(rows.error());
    for (const auto& row : *rows) {
      if (row.source != source_ || row.entity_type != entity_type) continue;
      const auto at = walk_.find(row.commit_sha);
      if (at != walk_.end()) return std::optional<std::size_t>{at->second};
    }
    return std::optional<std::size_t>{};
  }

  P::IStore& store_;
  P::Uuid source_;
  std::unordered_map<std::string, std::size_t> walk_;  // commit -> place in the import's walk order
};

}  // namespace

Result<ingest::AgeFn> make_age_fn(P::IStore& store, const SourceSettings& settings, const fs::path& scratch) {
  dvc::GitConfig git;
  git.repo = settings.path;
  git.branch = settings.branch;
  git.scratch = scratch;
  auto reader = dvc::GitReader::open(std::move(git));
  if (!reader) return fail(reader.error());
  auto order = reader->rev_list(std::nullopt);
  if (!order) return fail(order.error());
  std::unordered_map<std::string, std::size_t> walk;
  walk.reserve(order->size());
  for (std::size_t i = 0; i < order->size(); ++i) walk.emplace((*order)[i], i);
  return ingest::AgeFn(AgeAsOf(store, settings.uuid, std::move(walk)));
}

}  // namespace elctl::import_detail
