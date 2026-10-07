#pragma once

// The lab's flux monitor sets (flux fitting design, section 4): one
// `document` reference, so every client uses the same standards and changes
// are revisioned. A store with no document behaves as if it held the two
// default sets.
//
// And a level's inputs (section 6.1): its positions, holder geometry, monitor
// analyses and saved flux, with the options JSON a saved fit carries (6.4).
//
// And saving a fit (section 6.3): one changeset of flux_position revisions.

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/persistence/store.hpp"
#include "pychron/processing/flux_fit.hpp"
#include "pychron/processing/source.hpp"

namespace pychron::processing {

inline constexpr std::string_view kFluxMonitorsKey = "pychron/flux_monitors.json";

struct MonitorSets {
  std::string default_name;
  std::vector<MonitorSet> sets;
  std::string other_json = "{}";  // top-level keys this version does not know, kept as they were (unknown keys inside a monitor object are not)
  const MonitorSet* find(std::string_view name) const;  // empty name: the default
  friend bool operator==(const MonitorSets&, const MonitorSets&) = default;
};

// FC-2 after Kuiper et al. 2008 (the default) and after Renne et al. 1998.
MonitorSets default_monitor_sets();

// Error (Config, "flux monitors: ...") naming the key that is not valid.
Result<MonitorSets> parse_monitor_sets(std::string_view json);
std::string to_json(const MonitorSets& sets);

struct LoadedMonitorSets {
  MonitorSets sets;
  std::optional<persistence::Uuid> ref_object;  // nullopt: never saved
  std::optional<persistence::Uuid> head;
};

// A store with no document gives the defaults; a stored document that does
// not parse is an error, never the defaults.
Result<LoadedMonitorSets> load_monitor_sets(persistence::IStore& store);
// A new revision on top of `loaded.head` (the reference object is made on the
// first save). A Conflict outcome when someone saved in between.
Result<persistence::CommitOutcome> save_monitor_sets(persistence::IStore& store, const persistence::Actor& actor,
                                                     const MonitorSets& sets, const LoadedMonitorSets& loaded);

// ---- The options of a saved fit (design section 6.4) ------------------------

// `options_json` of a flux_position revision, as read.
struct FluxOptionsDoc {
  std::optional<FluxOptions> options;  // nullopt: no model_kind, or not one of the nine
  std::string monitor_set, monitor_sample;  // monitor_reference, monitor_sample
  std::optional<bool> used_in_fit;
  std::optional<bool> excluded;  // nullopt: a revision saved before the key existed, or imported
  std::optional<bool> all_positions;  // the fit's monitors were every position with analyses; nullopt likewise
  bool sd_replaced = false;  // F13: a least-squares model saved with SD reads as Msem
};

// Tolerant, never fails: a missing or mistyped key takes its default, and
// text that is not a JSON object is a document with nothing in it. Reads the
// legacy dict (model strings, "SEM", "SE but if MSWD>1 use SE * sqrt(MSWD)")
// and what flux_options_json writes.
FluxOptionsDoc parse_flux_options(std::string_view options_json);
// `used_in_fit` is information; `excluded` is true only for a monitor the
// user left out (FittedPosition::excluded), and is what a refit carries.
// `monitor_set.sample` and `all_positions` are how the fit's monitors were
// chosen, which the next load of the level repeats.
std::string flux_options_json(const FluxOptions& options, const MonitorSet& monitor_set, bool used_in_fit,
                              bool excluded, bool all_positions, double fit_mswd, int fit_dof,
                              std::string_view software);

// Whether two saved values are the same fit, so that saving one over the
// other would change nothing: every field equal, and the options equal as
// JSON (a jsonb column gives them back with its own key order and spacing)
// with `software` left out of both: the version that saved is no part of
// the fit. Options that are not JSON compare as text.
bool same_flux_value(const persistence::FluxValue& a, const persistence::FluxValue& b);

// ---- Loading a level (design section 6.1) -----------------------------------

// How the monitors of a level are chosen. What is not given is as the
// level's newest saved fit had it, so a saved fit is repeated (F9).
struct MonitorSelection {
  std::string monitor_set;  // empty: the saved fit's, else the document's default
  // The monitor sample. nullopt: the saved fit's when the set in use is the
  // saved fit's own, else the set's (a set named here that is not the saved
  // one, and the default standing in for a saved set the document lacks,
  // use their own sample).
  std::optional<std::string> sample;
  // true: every position that has analyses is a monitor; false: the
  // positions of the monitor sample; nullopt: by the sample when `sample`
  // is given, else as saved, else false.
  std::optional<bool> all_positions;
};

// The positions of a level that take part in a fit: the monitors (with their
// analyses, reduced, and F) and the unknowns (the other positions that have
// an identifier), each with its hole's x, y and its head flux revision.
// The monitors are chosen as `selection` says and, where it does not say,
// as the level's newest saved fit chose them (its `monitor_sample`, under
// its own monitor set only, and its `all_positions`);
// `LevelInputs::monitor_set.sample` and `all_positions` are what was used,
// and what a save of the fit writes.
// A position's hole is the holder hole whose ordinal is the position - 1 (a
// hole's id is only its label). Error (Config, "flux: ...") for an
// irradiation, level or monitor set that does not exist, an empty monitor
// sample name, a level with no holder and a position beyond the holder.
Result<LevelInputs> load_level(IAnalysisSource& source, persistence::IStore& store, std::string_view irradiation,
                               std::string_view level, const MonitorSelection& selection);

// ---- Saving a level (design section 6.3) ------------------------------------

struct SaveSelection {
  std::set<int> skip_positions;  // holes not to save
};

// (`SaveOutcome` is the revision sources' in revisions.hpp.)
struct FluxSaveOutcome {
  int written = 0, unchanged = 0, skipped = 0;
  std::optional<persistence::Conflict> conflict;  // set: nothing was written
  std::string conflict_position;                  // "hole 7"
};

// What a position of a fit is saved as: the model's J, a monitor's own mean
// and analyses, the monitor set's constants and the options of 6.4, with the
// position's `used_in_fit` and `excluded` and the level's MSWD and degrees
// of freedom. An analysis is saved omitted only when it was omitted by rule,
// never because it could not be reduced or gave no J.
persistence::FluxValue flux_value_of(const LevelFit& fit, const FittedPosition& position, std::string_view software);

// One `Reference` changeset, "fit flux for <irradiation><level>", with a
// revision for every position of the fit that is not skipped and whose value
// is not already its head's (same_flux_value: the software that saved does
// not count); a save that would write nothing commits nothing.
// Each head moves by compare-and-swap from the revision the level was loaded
// with (`FittedPosition::saved_revision`): a head someone moved since to a
// value other than the one being saved makes the save a conflict, of the
// lowest such hole, and nothing is written (a head moved to the same value
// is that position unchanged). A position that has no reference object yet
// gets one, scoped as the importer scopes it. Error (Config, "flux: ...") for an irradiation or level that
// does not exist, and, naming the lowest such hole and with nothing written,
// when a position to save has a J (or a mean J) that is not finite and above
// zero or an error of one that is not finite and at least zero.
Result<FluxSaveOutcome> save_level(persistence::IStore& store, const persistence::Actor& actor, const LevelFit& fit,
                                   const SaveSelection& selection, std::string_view software);

}  // namespace pychron::processing
