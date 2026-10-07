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

// The kinds of summary age pychron-cpp computes for a sample.
enum class AgeKind { Plateau, WeightedMean, Isochron, Integrated };
// The service's name for the kind of summary age, from its lookup list.
std::string_view age_type_name(AgeKind kind);

}  // namespace pychron::earthbank
