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
#include <string_view>
#include <vector>

#include "pychron/arar/reduce.hpp"
#include "pychron/core/error.hpp"
#include "pychron/processing/domain.hpp"
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

  pychron::arar::ArArSettings reduction() const;
  pychron::reduction::PlateauCriteria criteria() const;
};

// `args[i]` as one of the flags above. True: it was one, and is taken with
// its value (`i` is left on the last word taken). False: it is not one, and
// nothing was touched. Config: it is one and its value is missing or wrong.
pychron::Result<bool> take_selection_flag(Selection& selection, const std::vector<std::string>& args, std::size_t& i);
pychron::Result<bool> take_age_flag(AgeFlags& ages, const std::vector<std::string>& args, std::size_t& i);

// What only the whole selection can say: --to before --from.
pychron::Result<void> check_selection(const Selection& selection);

std::optional<int> parse_int(std::string_view text);

// How a store source opens under `domain`: the domain's reduction version
// and its extension loader (Ar-Ar's; noble_gas has none).
pychron::processing::StoreSourceOptions store_options(const pychron::processing::Domain& domain);
// store_options() of the Ar-Ar domain: `elctl publish` and `earthbank` are
// Ar-Ar's whatever the install.
pychron::processing::StoreSourceOptions arar_store_options();

// The store at `db_url`, for reading, opened with `options` on one
// connection. A command that reads does not make a store: a SQLite file that
// is not there, or is empty, is an error.
pychron::Result<std::unique_ptr<pychron::processing::StoreSource>> open_source(
    const std::string& db_url, pychron::processing::StoreSourceOptions options);

// Adds the node `select` that loads the selection, oldest analysis first.
pychron::Result<void> add_select(pychron::processing::Pipeline& pipeline, const pychron::processing::UnitRegistry& registry,
                                 const Selection& selection);

// The selected analyses, not reduced, oldest first. `warnings` gets what the
// load has to say (analyses that could not be read).
pychron::Result<std::vector<pychron::processing::AnalysisPtr>> load_selection(
    const Selection& selection, const std::string& db_url, pychron::processing::StoreSourceOptions options,
    std::vector<std::string>& warnings);

}  // namespace elctl
