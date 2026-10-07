#pragma once

// The lab's flux monitor sets (flux fitting design, section 4): one
// `document` reference, so every client uses the same standards and changes
// are revisioned. A store with no document behaves as if it held the two
// default sets.
//
// And a level's inputs (section 6.1): its positions, holder geometry, monitor
// analyses and saved flux, with the options JSON a saved fit carries (6.4).

#include <optional>
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
  bool sd_replaced = false;  // F13: a least-squares model saved with SD reads as Msem
};

// Tolerant, never fails: a missing or mistyped key takes its default, and
// text that is not a JSON object is a document with nothing in it. Reads the
// legacy dict (model strings, "SEM", "SE but if MSWD>1 use SE * sqrt(MSWD)")
// and what flux_options_json writes.
FluxOptionsDoc parse_flux_options(std::string_view options_json);
std::string flux_options_json(const FluxOptions& options, const MonitorSet& monitor_set, bool used_in_fit,
                              double fit_mswd, int fit_dof, std::string_view software);

// ---- Loading a level (design section 6.1) -----------------------------------

struct MonitorSelection {
  std::string monitor_set;            // empty: the saved fit's, else the document's default
  std::optional<std::string> sample;  // overrides the set's sample name
  bool all_positions = false;         // every position that has analyses is a monitor
};

// The positions of a level that take part in a fit: the monitors (with their
// analyses, reduced, and F) and the unknowns (the other positions that have
// an identifier), each with its hole's x, y and its head flux revision.
// A position's hole is the holder hole whose ordinal is the position - 1 (a
// hole's id is only its label). Error (Config, "flux: ...") for an
// irradiation, level or monitor set that does not exist, an empty monitor
// sample name, a level with no holder and a position beyond the holder.
Result<LevelInputs> load_level(IAnalysisSource& source, persistence::IStore& store, std::string_view irradiation,
                               std::string_view level, const MonitorSelection& selection);

}  // namespace pychron::processing
