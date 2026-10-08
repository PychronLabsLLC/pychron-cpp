// A level's inputs to a flux fit, read from the store (flux fitting design,
// section 6.1): the level sheet, the monitor set, the holder's geometry, the
// monitor analyses through the analysis source, and each position's head
// flux_position revision. And a fit written back (section 6.3) as one
// changeset of flux_position revisions.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <tuple>
#include <variant>

#include "pychron/processing/flux_store.hpp"
#include "pychron/processing/reduced.hpp"

namespace pychron::processing {

namespace ps = persistence;

namespace {

Unexpected<Error> bad(std::string what) { return fail(ErrorKind::Config, "flux: " + std::move(what)); }

// A position's head flux revision and when it became the head.
struct ReadFlux {
  SavedFlux saved;
  FluxOptionsDoc doc;
  ps::ChangeSeq change_seq = 0;
};

Result<std::optional<ReadFlux>> read_flux(ps::IStore& store, const ps::RefObjectRow& object) {
  if (!object.head) return std::optional<ReadFlux>{};
  auto payload = store.load_payload(*object.head);
  if (!payload) return fail(payload.error());
  const auto* ref = *payload ? std::get_if<ps::RefPayload>(&**payload) : nullptr;
  const auto* flux = ref ? std::get_if<ps::FluxValue>(ref) : nullptr;
  if (!flux) return fail(ErrorKind::Protocol, "flux: the reference " + object.key + " is not a flux position");

  ReadFlux out;
  SavedFlux& s = out.saved;
  s.revision = object.head->str();
  s.j = flux->j;
  s.j_err = flux->j_err;
  s.mean_j = flux->mean_j;
  s.mean_j_err = flux->mean_j_err;
  s.mean_j_mswd = flux->mean_j_mswd;
  if (flux->options_json) out.doc = parse_flux_options(*flux->options_json);
  s.options = out.doc.options;
  s.used_in_fit = out.doc.used_in_fit;
  s.excluded = out.doc.excluded;
  s.monitor_set = out.doc.monitor_set;
  for (const auto& a : flux->analyses)
    if (a.is_omitted) s.omitted.insert(a.record_id);

  auto history = store.history(object.uuid, ps::Kind::RefValue);
  if (!history) return fail(history.error());
  for (const auto& revision : *history) {
    if (revision.uuid != *object.head) continue;
    s.saved_by = revision.author_name;
    s.saved_utc = revision.changeset.created.iso();
    out.change_seq = revision.change_seq;
    break;
  }
  return std::optional<ReadFlux>{std::move(out)};
}

struct FoundLevel {
  ps::Uuid irradiation;
  ps::LevelRow level;
};

Result<FoundLevel> find_level(ps::IStore& store, const std::string& irradiation, const std::string& level) {
  auto irradiations = store.irradiations();
  if (!irradiations) return fail(irradiations.error());
  const auto irradiation_row = std::find_if(irradiations->begin(), irradiations->end(),
                                            [&](const ps::IrradiationRow& r) { return r.name == irradiation; });
  if (irradiation_row == irradiations->end()) return bad("no irradiation '" + irradiation + "'");
  auto levels = store.levels(irradiation_row->uuid);
  if (!levels) return fail(levels.error());
  const auto level_row =
      std::find_if(levels->begin(), levels->end(), [&](const ps::LevelRow& r) { return r.name == level; });
  if (level_row == levels->end()) return bad("no level " + level + " of " + irradiation);
  return FoundLevel{irradiation_row->uuid, *level_row};
}

std::string flux_key(const std::string& irradiation, const std::string& level, int hole) {
  return irradiation + "/" + level + "/" + std::to_string(hole);
}

// The head flux revision of each position of a level that has one, by hole.
Result<std::map<int, ReadFlux>> read_heads(ps::IStore& store, const ps::Uuid& irradiation_uuid,
                                           const std::string& irradiation, const std::string& level,
                                           const std::vector<ps::PositionRow>& positions) {
  auto flux_objects = store.ref_objects(ps::RefType::FluxPosition, irradiation_uuid);
  if (!flux_objects) return fail(flux_objects.error());
  std::map<std::string, const ps::RefObjectRow*> flux_by_key;
  for (const auto& object : *flux_objects) flux_by_key.emplace(object.key, &object);
  std::map<int, ReadFlux> out;
  for (const auto& p : positions) {
    const auto object = flux_by_key.find(flux_key(irradiation, level, p.position));
    if (object == flux_by_key.end()) continue;
    auto read = read_flux(store, *object->second);
    if (!read) return fail(read.error());
    if (*read) out.emplace(p.position, std::move(**read));
  }
  return out;
}

// Section 6.1 step 2: the set named; else the one the level's saved fit
// names, if the document has it; else the document's default (the caller
// is told: LevelInputs::saved_monitor_set_missing).
Result<MonitorSet> resolve_monitor_set(const MonitorSets& sets, const MonitorSelection& selection,
                                       const std::string& saved_name) {
  if (!selection.monitor_set.empty()) {
    if (const MonitorSet* named = sets.find(selection.monitor_set)) return *named;
    std::string available;
    for (const auto& s : sets.sets) available += (available.empty() ? "" : ", ") + s.name;
    return bad("no monitor set '" + selection.monitor_set + "' (available: " + available + ")");
  }
  if (!saved_name.empty())
    if (const MonitorSet* saved = sets.find(saved_name)) return *saved;
  if (const MonitorSet* fallback = sets.find("")) return *fallback;
  return bad("the monitor sets have no default");
}

// Every analysis of the level's `identifiers`, whatever its tag (a tag omits
// an analysis from the mean; it does not hide it).
Result<std::vector<AnalysisSummary>> browse_all(IAnalysisSource& source, const std::string& irradiation,
                                                const std::string& level, std::vector<std::string> identifiers) {
  std::vector<AnalysisSummary> rows;
  if (identifiers.empty()) return rows;
  BrowseQuery query;
  query.irradiations = {irradiation};
  query.levels = {level};
  query.identifiers = std::move(identifiers);
  query.exclude_tags.clear();
  for (;;) {
    auto page = source.browse(query);
    if (!page) return fail(page.error());
    rows.insert(rows.end(), page->rows.begin(), page->rows.end());
    if (!page->next) break;
    query.after = page->next;
  }
  std::sort(rows.begin(), rows.end(), [](const AnalysisSummary& a, const AnalysisSummary& b) {
    return std::tie(a.identifier, a.aliquot, a.increment, a.uuid) < std::tie(b.identifier, b.aliquot, b.increment, b.uuid);
  });
  return rows;
}

// F is 40Ar*/39ArK, which needs no J: the analysis is reduced without the
// position's saved flux, so a level with no flux yet, or one saved with a J
// that has no error, still gives its monitors' F.
Result<LevelAnalysis> reduce_monitor(IAnalysisSource& source, const AnalysisSummary& row) {
  auto loaded = source.load(row.uuid);
  if (!loaded) return fail(loaded.error());
  auto without_flux = std::make_shared<Analysis>(**loaded);
  without_flux->context.flux.reset();
  const ReducedPtr reduced = reduce_analysis(std::move(without_flux), ReductionSettings{});

  LevelAnalysis out;
  out.uuid = row.uuid;
  out.record_id = row.runid;
  out.tag = (*loaded)->tag;
  if (reduced->arar && reduced->arar->f.f && reduced->reduction_error.empty()) {
    out.f = *reduced->arar->f.f;
  } else {
    out.reduction_error = !reduced->reduction_error.empty() ? reduced->reduction_error : "F is undefined (no 39ArK)";
  }
  return out;
}

}  // namespace

Result<LevelInputs> load_level(IAnalysisSource& source, ps::IStore& store, std::string_view irradiation,
                               std::string_view level, const MonitorSelection& selection) {
  LevelInputs out;
  out.irradiation = std::string(irradiation);
  out.level = std::string(level);
  const std::string of_level = "level " + out.level + " of " + out.irradiation;

  // 1. The level sheet.
  auto level_found = find_level(store, out.irradiation, out.level);
  if (!level_found) return fail(level_found.error());
  const ps::Uuid irradiation_uuid = level_found->irradiation;
  const ps::LevelRow* level_row = &level_found->level;
  auto sheet = store.level_sheet(level_row->uuid);
  if (!sheet) return fail(sheet.error());
  if (!*sheet) return bad("no " + of_level);

  // 5. The holder's geometry. Position N is the hole with ordinal N - 1, as
  // on the entry sheet; a hole's id is only its label.
  if (!level_row->holder) return bad(of_level + " has no holder");
  out.holder = level_row->holder_name.value_or("");
  auto holder_head = store.head(*level_row->holder, ps::Kind::RefValue);
  if (!holder_head) return fail(holder_head.error());
  if (!*holder_head)
    return bad(out.holder.empty() ? of_level + " has no holder"
                                  : "holder " + out.holder + " of " + of_level + " has no geometry");
  auto holder_payload = store.load_payload(**holder_head);
  if (!holder_payload) return fail(holder_payload.error());
  const auto* holder_ref = *holder_payload ? std::get_if<ps::RefPayload>(&**holder_payload) : nullptr;
  const auto* holder = holder_ref ? std::get_if<ps::HolderValue>(holder_ref) : nullptr;
  if (!holder) return fail(ErrorKind::Protocol, "flux: the holder of " + of_level + " is not a holder");
  std::map<int, const ps::HolderHole*> holes;  // by ordinal
  for (const auto& h : holder->holes) holes.emplace(h.ordinal, &h);

  // 6. Each position's head flux revision. The newest one with options
  // gives the default options; the newest one that names a monitor set is
  // the level's saved fit, whose set, sample and all_positions go together
  // (R21): another revision's could be another standard's.
  auto heads = read_heads(store, irradiation_uuid, out.irradiation, out.level, (*sheet)->positions);
  if (!heads) return fail(heads.error());
  std::map<int, SavedFlux> saved;
  const ReadFlux* saved_fit = nullptr;
  const ReadFlux* newest_options = nullptr;
  for (auto& [hole, read] : *heads) {
    if (read.doc.options && (!newest_options || read.change_seq > newest_options->change_seq)) newest_options = &read;
    if (!read.doc.monitor_set.empty() && (!saved_fit || read.change_seq > saved_fit->change_seq)) saved_fit = &read;
  }
  if (newest_options) {
    out.saved_options = newest_options->doc.options;
    out.saved_sd_replaced = newest_options->doc.sd_replaced;
  }
  const std::string saved_set = saved_fit ? saved_fit->doc.monitor_set : std::string();
  const std::string saved_sample = saved_fit ? saved_fit->doc.monitor_sample : std::string();
  const bool saved_all_positions = saved_fit && saved_fit->doc.all_positions.value_or(false);
  for (auto& [hole, read] : *heads) saved.emplace(hole, std::move(read.saved));

  // 2. The monitor set.
  auto sets = load_monitor_sets(store);
  if (!sets) return fail(sets.error());
  auto set = resolve_monitor_set(sets->sets, selection, saved_set);
  if (!set) return fail(set.error());
  out.monitor_set = std::move(*set);
  out.saved_monitor_set = saved_set;
  out.saved_monitor_set_missing = !saved_set.empty() && !sets->sets.find(saved_set);
  // How the saved fit chose its monitors is repeated only under its own set
  // (R20, R22): every save writes the sample, and another set (one named, or
  // the default standing in for a set the document lacks) is another
  // standard, whose age must not be given to the saved fit's monitors.
  const bool under_saved_set = !saved_set.empty() && out.monitor_set.name == saved_set;
  if (selection.sample) {
    // An empty name would make a monitor of every position with no sample.
    if (selection.sample->empty()) return bad("the monitor sample name is empty");
    out.monitor_set.sample = *selection.sample;
  } else if (!saved_sample.empty() && under_saved_set) {
    // The saved fit's monitors were another sample's than its set's: again.
    out.monitor_set.sample = saved_sample;
  }
  // As asked; else by the sample when one is named (a named sample undoes a
  // saved all-positions fit); else as saved, under the saved fit's set only
  // (R22).
  out.all_positions = selection.all_positions ? *selection.all_positions
                                              : !selection.sample && under_saved_set && saved_all_positions;

  // 3. Monitors and unknowns. With all_positions the analyses decide.
  std::vector<std::string> identifiers;
  for (const auto& p : (*sheet)->positions) {
    if (!p.identifier) continue;
    if (out.all_positions || p.sample_name == out.monitor_set.sample) identifiers.push_back(*p.identifier);
  }
  // 4. The monitor analyses.
  auto rows = browse_all(source, out.irradiation, out.level, std::move(identifiers));
  if (!rows) return fail(rows.error());
  std::map<std::string, std::vector<LevelAnalysis>> analyses;  // by identifier
  for (const auto& row : *rows) {
    auto analysis = reduce_monitor(source, row);
    if (!analysis) return fail(analysis.error());
    analyses[row.identifier].push_back(std::move(*analysis));
  }

  for (const auto& p : (*sheet)->positions) {
    LevelPosition position;
    position.hole = p.position;
    position.position_uuid = p.uuid.str();
    position.identifier = p.identifier.value_or("");
    position.sample = p.sample_name;
    if (p.identifier)
      if (auto found = analyses.find(*p.identifier); found != analyses.end()) position.analyses = std::move(found->second);
    if (out.all_positions) {
      // Every position that has analyses is a monitor; there are no unknowns.
      if (position.analyses.empty()) continue;
      position.monitor = true;
    } else {
      position.monitor = p.sample_name == out.monitor_set.sample;
      if (!position.monitor && !p.identifier) continue;  // an empty hole
    }
    const auto hole = holes.find(p.position - 1);
    if (hole == holes.end())
      return bad("position " + std::to_string(p.position) + " of " + of_level + " is beyond " +
                 (out.holder.empty() ? "its holder" : "holder " + out.holder) + " (" +
                 std::to_string(holder->holes.size()) + " holes)");
    position.x = hole->second->x;
    position.y = hole->second->y;
    if (auto s = saved.find(p.position); s != saved.end()) position.saved = std::move(s->second);
    out.positions.push_back(std::move(position));
  }
  return out;
}

Result<std::vector<SavedPosition>> load_saved_flux(ps::IStore& store, std::string_view irradiation,
                                                   std::string_view level) {
  const std::string irradiation_name(irradiation), level_name(level);
  auto found = find_level(store, irradiation_name, level_name);
  if (!found) return fail(found.error());
  auto sheet = store.level_sheet(found->level.uuid);
  if (!sheet) return fail(sheet.error());
  if (!*sheet) return bad("no level " + level_name + " of " + irradiation_name);
  auto heads = read_heads(store, found->irradiation, irradiation_name, level_name, (*sheet)->positions);
  if (!heads) return fail(heads.error());
  std::vector<SavedPosition> out;
  for (const auto& p : (*sheet)->positions) {
    SavedPosition position;
    position.hole = p.position;
    position.identifier = p.identifier.value_or("");
    position.sample = p.sample_name;
    if (auto head = heads->find(p.position); head != heads->end()) position.saved = std::move(head->second.saved);
    if (position.identifier.empty() && position.sample.empty() && !position.saved) continue;  // an empty hole
    out.push_back(std::move(position));
  }
  std::sort(out.begin(), out.end(), [](const SavedPosition& a, const SavedPosition& b) { return a.hole < b.hole; });
  return out;
}

LevelFluxStatus level_flux_status(const ps::LevelSheet& sheet, std::string_view monitor_sample) {
  if (monitor_sample.empty()) return LevelFluxStatus::NoMonitors;  // a position with no sample has no name either
  bool any = false, all_have_j = true;
  for (const auto& position : sheet.positions) {
    if (position.sample_name != monitor_sample) continue;
    any = true;
    if (!position.j) all_have_j = false;
  }
  if (!any) return LevelFluxStatus::NoMonitors;
  return all_have_j ? LevelFluxStatus::Fitted : LevelFluxStatus::NotFitted;
}

ps::FluxValue flux_value_of(const LevelFit& fit, const FittedPosition& position, std::string_view software) {
  ps::FluxValue v;
  v.j = position.j;
  v.j_err = position.j_err;
  if (position.monitor) {
    v.mean_j = position.mean_j;
    v.mean_j_err = position.mean_j_err;
    v.mean_j_mswd = position.mean_j_mswd;
    for (const auto& a : position.analyses) v.analyses.push_back({ps::Uuid::parse(a.uuid), a.record_id, a.omitted});
    // Keyed by record id: the order the store reads them back in.
    std::sort(v.analyses.begin(), v.analyses.end(),
              [](const ps::FluxAnalysis& a, const ps::FluxAnalysis& b) { return a.record_id < b.record_id; });
  }
  const reduction::Measured lambda_k = fit.monitor_set.lambda_k();
  v.lambda_k_total = lambda_k.value;
  v.lambda_k_total_err = lambda_k.error;
  v.monitor_name = fit.monitor_set.name;
  v.monitor_material = fit.monitor_set.material;
  v.monitor_age = fit.monitor_set.age_ma;  // Ma, as the legacy level file has it
  v.monitor_age_err = fit.monitor_set.age_err_ma;
  v.options_json = flux_options_json(fit.options, fit.monitor_set, position.used_in_fit, position.excluded,
                                     fit.all_positions, fit.mswd, fit.dof, software);
  return v;
}

namespace {

// What of a value is no J to save: a J (or a monitor's mean J) that is not
// finite and above zero, or an error of one that is not finite and at least
// zero. A neighbour model can extrapolate to such a J.
std::optional<std::string> not_a_j(const ps::FluxValue& v) {
  const auto say = [](const char* name, double value) {
    char text[64];
    std::snprintf(text, sizeof text, "%s %g", name, value);
    return std::string(text);
  };
  const auto value_ok = [](double x) { return std::isfinite(x) && x > 0.0; };
  const auto error_ok = [](double x) { return std::isfinite(x) && x >= 0.0; };
  if (!v.j || !value_ok(*v.j)) return say("J", v.j.value_or(std::nan("")));
  if (!v.j_err || !error_ok(*v.j_err)) return say("J error", v.j_err.value_or(std::nan("")));
  if (v.mean_j && !value_ok(*v.mean_j)) return say("mean J", *v.mean_j);
  if (v.mean_j_err && !error_ok(*v.mean_j_err)) return say("mean J error", *v.mean_j_err);
  return std::nullopt;
}

}  // namespace

Result<FluxSaveOutcome> save_level(ps::IStore& store, const ps::Actor& actor, const LevelFit& fit,
                                   const SaveSelection& selection, std::string_view software) {
  // Before anything is written, a reference object included: every J to save is one.
  for (const FittedPosition& p : fit.positions) {
    if (selection.skip_positions.contains(p.hole)) continue;
    if (const auto what = not_a_j(flux_value_of(fit, p, software)))
      return bad("hole " + std::to_string(p.hole) + " of " + fit.irradiation + fit.level + " has " + *what +
                 ": nothing was saved (a J is finite and above zero, its error finite and not negative)");
  }

  auto level = find_level(store, fit.irradiation, fit.level);
  if (!level) return fail(level.error());
  auto objects = store.ref_objects(ps::RefType::FluxPosition, level->irradiation);
  if (!objects) return fail(objects.error());
  std::map<std::string, const ps::RefObjectRow*> by_key;
  for (const auto& object : *objects) by_key.emplace(object.key, &object);

  FluxSaveOutcome out;
  auto uow = store.begin(actor);
  if (!uow) return fail(uow.error());
  std::map<ps::Uuid, int> staged;  // reference object -> hole
  for (const FittedPosition& p : fit.positions) {
    if (selection.skip_positions.contains(p.hole)) {
      ++out.skipped;
      continue;
    }
    const std::string hole = "hole " + std::to_string(p.hole);
    ps::FluxValue value = flux_value_of(fit, p, software);
    const std::string key = flux_key(fit.irradiation, fit.level, p.hole);
    const auto existing = by_key.find(key);

    // What the head holds now is not written again, whoever wrote it.
    if (existing != by_key.end() && existing->second->head) {
      auto payload = store.load_payload(*existing->second->head);
      if (!payload) return fail(payload.error());
      const auto* ref = *payload ? std::get_if<ps::RefPayload>(&**payload) : nullptr;
      const auto* flux = ref ? std::get_if<ps::FluxValue>(ref) : nullptr;
      if (flux && same_flux_value(*flux, value)) {
        ++out.unchanged;
        continue;
      }
    }

    std::optional<ps::Uuid> expected;
    if (p.saved_revision) {
      expected = ps::Uuid::parse(*p.saved_revision);
      if (!expected) return bad("the saved revision of " + hole + " is not a uuid: " + *p.saved_revision);
    }
    ps::Uuid object;
    if (existing != by_key.end()) {
      object = existing->second->uuid;
    } else {
      ps::RefObjectSpec spec;
      spec.type = ps::RefType::FluxPosition;
      spec.key = key;
      spec.irradiation = level->irradiation;
      spec.level = level->level.uuid;
      spec.position = ps::Uuid::parse(p.position_uuid);
      if (!spec.position) return bad("the position of " + hole + " is not a uuid: " + p.position_uuid);
      auto made = store.add_ref_object(actor.client, spec);
      if (!made) return fail(made.error());
      object = *made;
    }
    auto revision =
        (*uow)->add_revision(object, ps::Kind::RefValue, ps::RevisionPayload{ps::RefPayload{std::move(value)}}, expected);
    if (!revision) return fail(revision.error());
    staged.emplace(object, p.hole);
  }
  if (staged.empty()) return out;

  auto committed = (*uow)->commit(ps::ChangesetKind::Reference, "fit flux for " + fit.irradiation + fit.level);
  if (!committed) return fail(committed.error());
  if (const auto* conflicts = std::get_if<std::vector<ps::Conflict>>(&*committed)) {
    int first = 0;
    for (const auto& conflict : *conflicts) {
      const auto hole = staged.find(conflict.subject);
      if (hole == staged.end() || (out.conflict && hole->second >= first)) continue;
      first = hole->second;
      out.conflict = conflict;
    }
    if (!out.conflict) return fail(ErrorKind::Protocol, "flux: the save conflicted on no position of the level");
    out.conflict_position = "hole " + std::to_string(first);
    return out;
  }
  out.written = static_cast<int>(staged.size());
  return out;
}

}  // namespace pychron::processing
