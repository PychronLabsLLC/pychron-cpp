// The age function of `elctl import verify` (legacy ingestion spec, 10.6,
// 10.30 and 10.32): the age of an analysis computed from the imported data as
// it stood when an interpreted age was saved.
//
// "As it stood" is a place in the walk of the project repository, not a time
// (git author dates tie and run out of order); the adapter of the source
// gives the place of a commit (ISourceAdapter::order_of). For each kind the
// reduction reads (intercepts, baselines, blanks, IC factors, tags) the
// revision used is the last one this source imported at or before the
// interpreted age's commit. A root revision has no provenance row of its
// own when it was made with the analysis: it then sits in the analysis's
// collection changeset and takes the place of the analysis's record. Any
// other revision without a provenance row of this source (an edit made in
// the store, another source's) has no place in the walk. The analysis is
// then built from the chosen payloads by the function the Data browser uses
// for the heads (processing::analysis_from_store) and reduced the same way
// (processing::reduce_analysis), so nothing of the reduction is repeated here.
//
// Reference data (flux, production, chronology, gains) comes from another
// source, whose walk cannot be compared with this one; it is taken as of the
// interpreted age's commit time t (AsOf::created). For each reference object
// the reduction uses, the revision is the last one in the object's chain
// whose changeset was created at or before t. The production is found
// through the level's link, which is revisioned itself: the link as of t
// names the production, and that production is taken as of t. An object that
// had no revision by t was not defined yet. A revision that states an
// absence (spec 10.21, "removed") is what the object held at t: no J, no
// production, no chronology, and never the value before it or after it.
// Gains play no part in the age: they are taken as of t when the object has
// a revision by then, and left out otherwise. Level geometry and sensitivity
// are not read by the reduction.
//
// Constants (decay constants, atmospheric ratios) are a preset of
// libs/reduction, the same for every analysis: the store keeps none per
// analysis. The preset is named in the basis of every age.
//
// Whatever cannot be reproduced is NotComparable with a fixed reason, never
// a number made from defaults:
//   as_of_commit_not_in_walk        the interpreted age's commit is not on the branch
//   analysis_not_placed             the analysis has no provenance row of this
//                                   source on the branch
//   analysis_imported_after         the analysis entered the repository after it
//   identity_changed_after          the analysis was renumbered after it: its
//                                   irradiation position may be another one
//   revision_outside_source         a revision without a place in the walk lies
//                                   before the one that would be used
//   no_intercepts                   nothing to reduce at that point
//   reference_not_yet_defined       the flux, the level's production link, the
//                                   production it named or the chronology had
//                                   no revision by the age's commit time
//   no_j, no_production, no_chronology
//                                   the analysis has no such reference, or
//                                   what it had at that time says it was
//                                   removed
//   not_reducible                   the reduction refused its input
//   age_undefined                   no age came out (1 + J F <= 0, no F)

#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#include "import_impl.hpp"
#include "pychron/processing/reduced.hpp"
#include "pychron/processing/store_source.hpp"

namespace elctl::import_detail {

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
  AgeAsOf(P::IStore& store, P::Uuid source, ingest::ISourceAdapter& adapter,
          pychron::reduction::ConstantsPreset constants)
      : store_(store), source_(source), adapter_(adapter), constants_(constants) {}

  Result<ParityAge> operator()(P::Uuid analysis, const ingest::AsOf& as_of) const {
    const auto no = [](const char* reason) { return ParityAge{NotComparable{reason}}; };
    if (as_of.source != source_) return no("other_source");
    auto at = order_of(as_of.commit);
    if (!at) return fail(at.error());
    if (!*at) return no("as_of_commit_not_in_walk");
    const std::int64_t limit = **at;

    // Where the analysis itself entered the walk: the place of the revisions
    // made with it.
    auto collected = place_of(analysis, "analysis");
    if (!collected) return fail(collected.error());
    if (!*collected) return no("analysis_not_placed");
    if (**collected > limit) return no("analysis_imported_after");
    // The changeset that made the analysis: the one of its signals root.
    std::optional<P::Uuid> collection;
    {
      auto signals = store_.history(analysis, P::Kind::Signals);
      if (!signals) return fail(signals.error());
      for (const auto& revision : *signals)
        if (!revision.parent) collection = revision.changeset.uuid;
    }

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
      std::int64_t chosen_place = 0;
      bool unplaced_before = false, unplaced = false;
      for (const auto& revision : *history) {
        auto place = place_of(revision.uuid, "revision");
        if (!place) return fail(place.error());
        std::optional<std::int64_t> where = *place;
        // A root made with the analysis. A first revision of a kind made
        // later in the store is parentless too, and is not this.
        if (!where && !revision.parent && collection && revision.changeset.uuid == *collection) where = *collected;
        if (!where) {
          unplaced = true;  // an edit made in the store, or by another source
          continue;
        }
        if (*where > limit || (chosen && *where < chosen_place)) continue;
        chosen = revision.uuid;
        chosen_place = *where;
        unplaced_before = unplaced;
      }
      // Also when nothing could be chosen and an unplaced revision exists:
      // whether it was there at the interpreted age cannot be told.
      if (unplaced_before || (!chosen && unplaced)) return no("revision_outside_source");
      if (!chosen) continue;  // the analysis had none of this kind then
      auto payload = store_.load_payload(*chosen);
      if (!payload) return fail(payload.error());
      if (!*payload) continue;
      parts.heads.insert_or_assign(kind, std::move(**payload));
      parts.head_revisions.insert_or_assign(kind, *chosen);
    }
    if (!parts.heads.contains(P::Kind::Intercepts)) return no("no_intercepts");

    // Reference data as of the age's commit time (spec 10.32). resolve_refs
    // says which objects the analysis is scoped to; which revision of each is
    // decided here, so pins and heads play no part.
    auto refs = store_.resolve_refs(analysis, P::RefPolicy{false});
    if (!refs) return fail(refs.error());
    for (const auto& ref : refs->refs) {
      const bool required = ref.type == P::RefType::FluxPosition || ref.type == P::RefType::Chronology ||
                            ref.type == P::RefType::LevelProduction;
      if (!required && ref.type != P::RefType::Gains) continue;  // not read, or (a production) found through its link
      auto value = value_as_of(ref.ref_object, as_of.created);
      if (!value) return fail(value.error());
      if (!*value) {
        if (required) return no("reference_not_yet_defined");
        continue;
      }
      const auto* link = std::get_if<P::LevelProductionValue>(&**value);
      if (!link) {
        parts.refs.push_back(std::move(**value));
        continue;
      }
      // The production the level named then, as it stood then.
      auto production = value_as_of(link->production, as_of.created);
      if (!production) return fail(production.error());
      if (!*production) return no("reference_not_yet_defined");
      parts.refs.push_back(std::move(**production));
    }

    auto built = processing::analysis_from_store(parts);
    if (!built) return no("not_reducible");
    if (!built->context.flux) return no("no_j");
    if (!built->context.production) return no("no_production");
    if (built->context.chronology.empty()) return no("no_chronology");
    processing::ReductionSettings reduction;
    reduction.preset = constants_;
    const auto reduced =
        processing::reduce_analysis(std::make_shared<const processing::Analysis>(std::move(*built)), reduction);
    if (!reduced || !reduced->arar) return no("not_reducible");
    if (!reduced->arar->ages) return no("age_undefined");
    // Both errors: the verifier compares the one the legacy file stored.
    const auto& age = reduced->arar->ages->age;                // J without its error
    const auto& with_j = reduced->arar->ages->age_w_j_err;
    if (!std::isfinite(age.nominal()) || !std::isfinite(age.std_dev()) || !std::isfinite(with_j.std_dev()))
      return no("age_undefined");
    ingest::ComputedAge computed;
    computed.age = age.nominal();
    computed.age_err = age.std_dev();
    computed.age_err_w_j = with_j.std_dev();
    computed.basis = "constants=" + std::string(pychron::reduction::to_string(constants_));
    return ParityAge{std::move(computed)};
  }

 private:
  // The revisions of a reference object in chain order: each after its
  // parent. The store lists them by change sequence, which is the same unless
  // a changeset got revisions in more than one batch.
  static std::vector<P::RevisionInfo> chain_of(std::vector<P::RevisionInfo> revisions) {
    std::map<P::Uuid, const P::RevisionInfo*> child_of;
    const P::RevisionInfo* root = nullptr;
    for (const auto& revision : revisions) {
      if (!revision.parent) {
        if (root) return revisions;  // not one chain: as the store lists them
        root = &revision;
      } else if (!child_of.emplace(*revision.parent, &revision).second) {
        return revisions;
      }
    }
    std::vector<P::RevisionInfo> chain;
    for (const P::RevisionInfo* at = root; at;) {
      chain.push_back(*at);
      const auto next = child_of.find(at->uuid);
      at = next == child_of.end() ? nullptr : next->second;
    }
    return chain.size() == revisions.size() ? chain : revisions;
  }

  // What a reference object held at time `t`: the payload of the last
  // revision of its chain whose changeset was created at or before `t`.
  // nullopt: it had no revision by then. The chain is read once per run.
  Result<std::optional<P::RefPayload>> value_as_of(P::Uuid object, P::UtcTime t) const {
    auto known = chains_->find(object);
    if (known == chains_->end()) {
      auto history = store_.history(object, P::Kind::RefValue);
      if (!history) return fail(history.error());
      known = chains_->emplace(object, chain_of(std::move(*history))).first;
    }
    std::optional<P::Uuid> chosen;
    for (const auto& revision : known->second)
      if (revision.changeset.created <= t) chosen = revision.uuid;
    if (!chosen) return std::optional<P::RefPayload>{};
    auto payload = store_.load_payload(*chosen);
    if (!payload) return fail(payload.error());
    const auto* value = *payload ? std::get_if<P::RefPayload>(&**payload) : nullptr;
    if (!value) return fail(ErrorKind::Protocol, "reference revision " + chosen->str() + " has no reference value");
    return std::optional<P::RefPayload>{*value};
  }

  // The place in the walk of the commit this source imported `entity` at;
  // nullopt: it has no provenance row of this source, or the commit is not
  // on the branch.
  Result<std::optional<std::int64_t>> place_of(P::Uuid entity, std::string_view entity_type) const {
    auto rows = store_.provenance_for(entity);
    if (!rows) return fail(rows.error());
    for (const auto& row : *rows) {
      if (row.source != source_ || row.entity_type != entity_type) continue;
      auto at = order_of(row.commit_sha);
      if (!at) return fail(at.error());
      if (*at) return *at;
    }
    return std::optional<std::int64_t>{};
  }

  // The adapter's answer, asked once per commit.
  Result<std::optional<std::int64_t>> order_of(const std::string& commit) const {
    if (const auto known = places_->find(commit); known != places_->end()) return known->second;
    auto place = adapter_.order_of(commit);
    if (!place) return fail(place.error());
    places_->emplace(commit, *place);
    return *place;
  }

  P::IStore& store_;
  P::Uuid source_;
  ingest::ISourceAdapter& adapter_;
  // commit -> place in the import's walk order; shared by the copies std::function makes
  std::shared_ptr<std::unordered_map<std::string, std::optional<std::int64_t>>> places_ =
      std::make_shared<std::unordered_map<std::string, std::optional<std::int64_t>>>();
  // reference object -> its revisions in chain order; shared like places_
  std::shared_ptr<std::map<P::Uuid, std::vector<P::RevisionInfo>>> chains_ =
      std::make_shared<std::map<P::Uuid, std::vector<P::RevisionInfo>>>();
  pychron::reduction::ConstantsPreset constants_;
};

}  // namespace

ingest::AgeFn make_age_fn(P::IStore& store, P::Uuid source, ingest::ISourceAdapter& adapter,
                          pychron::reduction::ConstantsPreset constants) {
  return ingest::AgeFn(AgeAsOf(store, source, adapter, constants));
}

}  // namespace elctl::import_detail
