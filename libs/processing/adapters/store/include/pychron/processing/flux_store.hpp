#pragma once

// The lab's flux monitor sets (flux fitting design, section 4): one
// `document` reference, so every client uses the same standards and changes
// are revisioned. A store with no document behaves as if it held the two
// default sets.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/persistence/store.hpp"
#include "pychron/processing/flux_fit.hpp"

namespace pychron::processing {

inline constexpr std::string_view kFluxMonitorsKey = "pychron/flux_monitors.json";

struct MonitorSets {
  std::string default_name;
  std::vector<MonitorSet> sets;
  std::string other_json = "{}";  // keys this version does not know, kept as they were
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

}  // namespace pychron::processing
