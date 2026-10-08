// EarthBank's controlled vocabularies, as far as they can be known without
// asking the service.
//
// Many fields of a record (model.hpp) are controlled terms: the service keeps
// a list (the minerals, the Ar-Ar methods, the decay constants, ...) and a
// record refers to an entry by its id. A term is carried here by its name,
// in `<term>Name`; the id goes into `<term>Id` once the name has been looked
// up. This header holds what is fixed: which list each name field is looked
// up in, the one list that cannot be looked up at all (the uncertainty
// types), and the names pychron-cpp itself chooses (the method, the decay
// constant, the air ratio, the kind of age). The names are the service's
// own, as its Ar-Ar template workbook lists them (sheet `Lookup Tables`).
// Nothing here talks to the service.
#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace pychron::earthbank {

// Where one `<term>Name` field is resolved: the field that takes the id and
// the endpoint listing the vocabulary. An empty endpoint means there is no
// list to ask: the name is sent as it is.
struct LookupSpec {
  std::string_view name_field, id_field, endpoint;
};

// One per name field that is resolved against the service (the legacy
// integration's table, entry for entry), in no particular order. A field name
// is the same in every record that has it. Three `*Name` fields are in
// neither this table nor the next, because they are free text and not terms:
// `aliquotName` (the aliquot's own name), `locationName` (the name given to
// the place) and `lithologyName` (sent by name; the legacy integration never
// looked it up).
std::span<const LookupSpec> lookup_specs();

// The uncertainty type fields (the sigma level a number is quoted at). Their
// list has no endpoint of its own, and the one that looks like it,
// `/api/core/l-error-types`, holds ids the data point's foreign key refuses:
// these are resolved by uncertainty_type_id and never asked for. `endpoint`
// is empty in each.
std::span<const LookupSpec> uncertainty_type_specs();

// The id of an uncertainty type: 1 for one sigma (`1 sigma`, `1s`, `1`, and
// pychron's error kinds `SE`, `SEM`, `SD`, `MSEM`, all estimates at that
// level), 2 for two sigma (`2 sigma`, `2s`, `2`). Case, spaces and
// punctuation are ignored. Anything else has no id.
std::optional<int> uncertainty_type_id(std::string_view name);

// The service's name for how the argon was extracted. Analyses with no
// heating step are a laser total fusion. With steps, the largest extraction
// value decides: at most 50 (watts or percent) is a laser, at least 300
// (degrees) a furnace, anything between, or no value, is left undefined.
std::string ar_method(bool any_step, std::optional<double> max_extract_value);

// The service's name for the 40K decay constant whose total (lambda_b +
// lambda_e, per year) this is, and for the atmospheric 40Ar/36Ar; matched to
// one part in 10^4. Empty when no named value matches.
std::string decay_constant_name(double lambda_total_per_year);
std::string air_ratio_name(double atm4036);

// A citation as people type it (`Min (2008)`, `Steiger and Jager (1977)`,
// `Renne et al. 2011`) as the service's name for that decay constant (`decay`
// true) or air ratio (false): the first author and the year pick the entry.
// A string that picks none is returned as it came, for the lookup to judge.
std::string normalize_citation(std::string_view override_text, bool decay);

// What a flux monitor is looked up by: `<name> <age> Ma`, or whichever part
// there is. The service's entries are long (`Fish Canyon Tuff Sanidine (FCTs)
// from Kuiper et al. 2008 (28.201 Ma)`), so this is a query, not a name.
std::string flux_monitor_query(std::string_view name, std::optional<double> age_ma);

// ---- A value against a list -----------------------------------------------
//
// The workbook route has no service to resolve a name: what is written is
// what the website's importer gets. The template carries the service's lists
// (its `Lookup Tables` sheet), and these say how a value stands to one, with
// no I/O: the exporter (xlsx.hpp) reads the list and asks.

// `entry` is what to write: for Exact the value as it was given, for
// Respelled the list's entry (less the spaces around it); empty otherwise.
struct ListMatch {
  enum class Kind {
    Exact,      // the list has it as given, or as given but for spaces around its entry
    Respelled,  // one entry is the value in another spelling
    Ambiguous,  // more than one entry could be meant: nothing is rewritten
    None,       // no entry is the value
  };
  Kind kind = Kind::None;
  std::string entry;
};

// `value` among `entries`. Another spelling is the same text but for the case
// of ASCII letters and the spaces around it, nothing else: not a part of an
// entry, not other spaces inside one. Two entries that differ only so are
// one when they are the same once trimmed; when they differ in case, a value
// that is neither exactly is Ambiguous.
ListMatch match_list_entry(std::string_view value, std::span<const std::string> entries);

// A flux monitor among the list's entries, which are long (`Fish Canyon Tuff
// Sanidine (FCTs) from Kuiper et al. 2008 (28.201 Ma)`) where a lab says
// `FC-2 28.201 Ma` (flux_monitor_query). First as match_list_entry. Then by
// name and age:
//   - the value's age is its number followed by `Ma`, else its last number;
//     an entry's is the `(28.201 Ma)` it ends with. They must be the same
//     number (28.2010 is 28.201; 28.2 and 28.20 are not): among a monitor's
//     entries the age decides, and a value without one matches nothing;
//   - the name is the rest. If it holds one of flux_monitor_aliases() it
//     means that monitor, and only that monitor's entries are looked at
//     (names of two monitors match nothing). If it holds none, every word of
//     it must be a word of the entry, one of them of three letters or more or
//     the entry's abbreviation in parentheses.
//   - either way, a mineral the name says must be the entry's
//     (flux_monitor_minerals()): `Fish Canyon biotite 28.201 Ma` is not the
//     Fish Canyon sanidine of that age, whatever else of it fits. A name that
//     says no mineral is held to none. And a name that holds the designation
//     of a split the list has not (`FCT-3`, the tuff's biotite) matches
//     nothing, though part of it is another monitor's name.
// One entry left: Respelled. Several: Ambiguous. None: None.
ListMatch match_flux_monitor(std::string_view value, std::span<const std::string> entries);

// A name pychron labs give a monitor, and the mark by which the list's
// entries for that monitor are known (the abbreviation each has in
// parentheses, or begins with). Built from the list in the Ar-Ar template:
// only monitors that are in it, and a test holds the two together. Compared
// without case or punctuation (`FC-2`, `fc2`), as whole words.
struct MonitorAlias {
  std::string_view alias, listed;
};
std::span<const MonitorAlias> flux_monitor_aliases();

// A word of the list's entries that is a mineral, lower case, and the
// mineral it is: the list's own spellings, its `Saniditine` among them. Only
// the minerals the list holds; a test holds the two together.
struct MonitorMineral {
  std::string_view word, mineral;
};
std::span<const MonitorMineral> flux_monitor_minerals();

// Terms the mapping writes that never vary, spelled as the template's lists
// have them: the unit of every intensity, the unit of every uncertainty
// (absolute, never percent), the sigma level every uncertainty is quoted at,
// the two answers in a "step included" column (the list's third, `N/A`, is
// never written: a column that does not apply is left empty), the unit of an
// aliquot's weight (free text in the template: "mg, g or grain/s"), the tool
// that made the interpretation, and the interpretation when the lab gives
// none.
inline constexpr std::string_view kIntensityUnits = "fA";
inline constexpr std::string_view kAbsoluteUncertainty = "abs.";
inline constexpr std::string_view kOneSigma = "1 sigma";
inline constexpr std::string_view kYes = "Yes";
inline constexpr std::string_view kNo = "No";
inline constexpr std::string_view kMilligrams = "mg";
inline constexpr std::string_view kInterpretationTool = "Pychron";
inline constexpr std::string_view kUnknownInterpretation = "Unknown";

// The kinds of summary age pychron-cpp computes for a sample.
enum class AgeKind { Plateau, WeightedMean, Isochron, Integrated };
// The service's name for the kind of summary age, from its lookup list.
std::string_view age_type_name(AgeKind kind);

}  // namespace pychron::earthbank
