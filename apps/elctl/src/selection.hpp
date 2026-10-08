#pragma once

// Which analyses a command reads from the DVC store and how their ages are
// made: the flags `elctl export` and `elctl earthbank` share, parsed by one
// piece of code, so the two commands select the same analyses and report the
// same ages for the same words.
//
// Selection (repeat a flag for several values; every flag narrows):
//   --sample <name>  --identifier <labnumber>  --project <name>
//   --irradiation <name>  --type <analysis type>  --uuid <analysis uuid>
//   --from <YYYY-MM-DD>  --to <YYYY-MM-DD>     UTC, inclusive
//   --include-invalid                           keep analyses tagged invalid
//   --limit <n>                                 at most n analyses (default 5000)
//   Without --type the unknowns are selected.
//
// Ages:
//   --constants default|legacy|legacy_preferences
//   --decay-error   propagate the decay-constant uncertainty into the ages
//   --plateau fleck|mahon  --plateau-steps <n>  --plateau-gas <percent>
//
// A command walks its arguments once, from the left, and offers each word to
// take_selection_flag and take_age_flag before it looks for a flag of its
// own: the first word that is wrong is the one reported, whoever parses it.
//
// Built only with persistence, like the commands that use it.

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/processing/model.hpp"
#include "pychron/processing/reduced.hpp"
#include "pychron/processing/store_source.hpp"
#include "pychron/processing/units.hpp"
#include "pychron/reduction/stats.hpp"

namespace elctl {

struct Selection {
  std::vector<std::string> samples, identifiers, projects, irradiations, types, uuids;
  std::optional<double> from, to;  // UTC epoch seconds
  bool include_invalid = false;
  int limit = 5000;
};

struct AgeFlags {
  std::string constants = "default";
  bool decay_error = false;
  std::string plateau = "fleck";
  int plateau_steps = 3;
  double plateau_gas = 50.0;

  pychron::processing::ReductionSettings reduction() const;
  pychron::reduction::PlateauCriteria criteria() const;
};

// `args[i]` as one of the flags above. True: it was one, and is taken with
// its value (`i` is left on the last word taken). False: it is not one, and
// nothing was touched. Config: it is one and its value is missing or wrong.
pychron::Result<bool> take_selection_flag(Selection& selection, const std::vector<std::string>& args, std::size_t& i);
pychron::Result<bool> take_age_flag(AgeFlags& ages, const std::vector<std::string>& args, std::size_t& i);

// What only the whole selection can say: --to before --from.
pychron::Result<void> check_selection(const Selection& selection);

std::optional<int> parse_int(const std::string& text);
std::optional<double> parse_double(const std::string& text);

// The store at `db_url`, for reading. A command that reads does not make a
// store: a SQLite file that is not there, or is empty, is an error.
pychron::Result<std::unique_ptr<pychron::processing::StoreSource>> open_source(const std::string& db_url);

// Adds the node `select` that loads the selection, oldest analysis first.
pychron::Result<void> add_select(pychron::processing::Pipeline& pipeline, const pychron::processing::UnitRegistry& registry,
                                 const Selection& selection);

// The selected analyses, not reduced, oldest first. `warnings` gets what the
// load has to say (analyses that could not be read).
pychron::Result<std::vector<pychron::processing::AnalysisPtr>> load_selection(const Selection& selection,
                                                                              const std::string& db_url,
                                                                              std::vector<std::string>& warnings);

}  // namespace elctl
