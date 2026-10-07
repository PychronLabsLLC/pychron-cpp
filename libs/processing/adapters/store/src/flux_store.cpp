// A level's inputs to a flux fit, read from the store (flux fitting design,
// section 6.1): the level sheet, the monitor set, the holder's geometry, the
// monitor analyses through the analysis source, and each position's head
// flux_position revision.

#include <algorithm>
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

// Section 6.1 step 2: the set named; else the one the level's saved fit
// names, if the document has it; else the document's default.
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
  auto irradiations = store.irradiations();
  if (!irradiations) return fail(irradiations.error());
  const auto irradiation_row = std::find_if(irradiations->begin(), irradiations->end(),
                                            [&](const ps::IrradiationRow& r) { return r.name == irradiation; });
  if (irradiation_row == irradiations->end()) return bad("no irradiation '" + out.irradiation + "'");
  auto levels = store.levels(irradiation_row->uuid);
  if (!levels) return fail(levels.error());
  const auto level_row =
      std::find_if(levels->begin(), levels->end(), [&](const ps::LevelRow& r) { return r.name == level; });
  if (level_row == levels->end()) return bad("no " + of_level);
  auto sheet = store.level_sheet(level_row->uuid);
  if (!sheet) return fail(sheet.error());
  if (!*sheet) return bad("no " + of_level);

  // 5. The holder's geometry, by hole id.
  if (!level_row->holder) return bad(of_level + " has no holder");
  out.holder = level_row->holder_name.value_or("");
  auto holder_head = store.head(*level_row->holder, ps::Kind::RefValue);
  if (!holder_head) return fail(holder_head.error());
  if (!*holder_head) return bad("holder " + out.holder + " of " + of_level + " has no geometry");
  auto holder_payload = store.load_payload(**holder_head);
  if (!holder_payload) return fail(holder_payload.error());
  const auto* holder_ref = *holder_payload ? std::get_if<ps::RefPayload>(&**holder_payload) : nullptr;
  const auto* holder = holder_ref ? std::get_if<ps::HolderValue>(holder_ref) : nullptr;
  if (!holder) return fail(ErrorKind::Protocol, "flux: holder " + out.holder + " is not a holder");
  std::map<std::string, const ps::HolderHole*> holes;
  for (const auto& h : holder->holes) holes.emplace(h.hole_id, &h);

  // 6. Each position's head flux revision; the most recent one with options
  // is the level's last fit.
  auto flux_objects = store.ref_objects(ps::RefType::FluxPosition, irradiation_row->uuid);
  if (!flux_objects) return fail(flux_objects.error());
  std::map<std::string, const ps::RefObjectRow*> flux_by_key;
  for (const auto& object : *flux_objects) flux_by_key.emplace(object.key, &object);
  std::map<int, SavedFlux> saved;
  std::string saved_set;
  ps::ChangeSeq options_seq = 0, set_seq = 0;
  for (const auto& p : (*sheet)->positions) {
    const auto object = flux_by_key.find(out.irradiation + "/" + out.level + "/" + std::to_string(p.position));
    if (object == flux_by_key.end()) continue;
    auto read = read_flux(store, *object->second);
    if (!read) return fail(read.error());
    if (!*read) continue;
    if ((*read)->doc.options && (!out.saved_options || (*read)->change_seq > options_seq)) {
      out.saved_options = (*read)->doc.options;
      out.saved_sd_replaced = (*read)->doc.sd_replaced;
      options_seq = (*read)->change_seq;
    }
    if (!(*read)->doc.monitor_set.empty() && (saved_set.empty() || (*read)->change_seq > set_seq)) {
      saved_set = (*read)->doc.monitor_set;
      set_seq = (*read)->change_seq;
    }
    saved.emplace(p.position, std::move((*read)->saved));
  }

  // 2. The monitor set.
  auto sets = load_monitor_sets(store);
  if (!sets) return fail(sets.error());
  auto set = resolve_monitor_set(sets->sets, selection, saved_set);
  if (!set) return fail(set.error());
  out.monitor_set = std::move(*set);
  if (selection.sample) out.monitor_set.sample = *selection.sample;

  // 3. Monitors and unknowns. With all_positions the analyses decide.
  std::vector<std::string> identifiers;
  for (const auto& p : (*sheet)->positions) {
    if (!p.identifier) continue;
    if (selection.all_positions || p.sample_name == out.monitor_set.sample) identifiers.push_back(*p.identifier);
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
    if (selection.all_positions) {
      // Every position that has analyses is a monitor; there are no unknowns.
      if (position.analyses.empty()) continue;
      position.monitor = true;
    } else {
      position.monitor = p.sample_name == out.monitor_set.sample;
      if (!position.monitor && !p.identifier) continue;  // an empty hole
    }
    const auto hole = holes.find(std::to_string(p.position));
    if (hole == holes.end())
      return bad("hole " + std::to_string(p.position) + " is not on holder " + out.holder + " (" + of_level + ")");
    position.x = hole->second->x;
    position.y = hole->second->y;
    if (auto s = saved.find(p.position); s != saved.end()) position.saved = std::move(s->second);
    out.positions.push_back(std::move(position));
  }
  return out;
}

}  // namespace pychron::processing
