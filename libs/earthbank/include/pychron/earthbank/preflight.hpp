// The check before an upload: would the service know every term?
//
// EarthBank is in production and takes nothing back, so what can be found
// out by asking is found out first. preflight walks the records an upload
// would send, exactly those (the Submissions the mapping built), and looks up
// every controlled term in them. It reports each one the service does not
// know, and separately each one it could not ask about, so that a run without
// a login does not show as misses the terms of the lists that want one. It
// passes on what the mapping noted about each sample, so that the user reads
// one report. It writes nothing: every request is a GET of a list, no package
// is needed, and the submissions are not changed.
//
// An id that is written cannot be corrected, so the report also says what it
// took a value for wherever that is not the value as it was written (a flux
// monitor found by its name and age, an entry under the lowest of two ids),
// and what it did not check at all, so that a clean report is read for what
// it is.
//
// Which fields are terms is read off the model's field lists, not kept by
// hand: a `<term>Name` field of a record that also has `<term>Id` is checked
// unless it is known to be free text (term_fields).
//
// No clock and no thread.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/earthbank/lookup.hpp"
#include "pychron/earthbank/model.hpp"

namespace pychron::earthbank {

// How one name field with an id beside it is treated.
enum class TermKind {
  Listed,    // looked up in a list of the service (lookup_specs, with an endpoint)
  Fixed,     // an uncertainty type: from the fixed table, never asked for
  ByName,    // sent by its name: the service keeps no list to ask (lookup_specs, no endpoint)
  FreeText,  // not a term, though an id is beside it: the name of the sample's place, the lithology
  Unlisted,  // in none of the tables: a field added to the model and not yet decided. Reported, not passed.
};

// One `<term>Name` field of a record that has `<term>Id` too. `record` is the
// service's name for the record: `Sample`, `Location`, `ArArDataPoint`,
// `ArArAliquot`, `ArArMeasurement`, `ArArAgeCalc`, `ArArAgeSummary`.
struct TermField {
  std::string_view record, name_field, id_field;
  TermKind kind = TermKind::Unlisted;
  std::string_view endpoint;  // Listed: the list
};

// Every such field of every record, in the order the records are sent and,
// within a record, the order of the service's field list.
std::vector<TermField> term_fields();

// A term the upload could not send as an id. `sample` is the sample's name
// (the data point's when it has none), `record` as in TermField, `endpoint`
// the list asked (empty for an uncertainty type). `outcome` is Unknown or
// CouldNotAsk and `detail` the resolver's sentence for it. The same value in
// the same field of several records of one kind of a sample (its
// measurements) is one Miss, and `count` says of how many.
struct Miss {
  std::string sample, record, field, value, endpoint;
  LookupOutcome outcome = LookupOutcome::Unknown;
  std::string detail;
  std::size_t count = 1;
};

// A term that was found, where the user should see what it was found as:
// the entry (`matched_name`) is not `value` but for the case of its letters,
// or the resolver has something to say of how it was found (`detail`: by a
// reference, under the lowest of several ids; else empty). One line for each
// value of a field of a kind of record, over all the submissions: `sample` is
// the first sample that has it, and `count` of how many records it is, of
// every sample.
struct Resolved {
  std::string sample, record, field, value, matched_name, detail;
  std::size_t count = 1;
};

// Something an upload sends that this check cannot check. `field` is
// `setup_name` or `irradiation_name` (Submission: the service has no list of
// either that can be asked) or `lithologyName` (sent by its name, as legacy
// sent it; whether the service wants an id there is not known). `reason` is
// a sentence for the user.
struct NotChecked {
  std::string field, reason;
};

// What the mapping noted on one submission (Submission::notes), unchanged.
struct SampleNotes {
  std::string sample;
  std::vector<std::string> notes;
};

struct PreflightReport {
  std::vector<Miss> misses;        // in the order found: by sample, record, field
  std::vector<SampleNotes> notes;  // the samples that have any, in order
  std::vector<Resolved> resolved;  // in the order first met
  // One for each of the three fields that some submission has a value in:
  // set-up, irradiation, lithology, in that order.
  std::vector<NotChecked> not_checked;

  // Nothing unknown and nothing that could not be asked. Notes do not count,
  // and nor does what was resolved or not checked: those are to be read.
  bool clean() const { return misses.empty(); }
  std::vector<Miss> unknown() const;
  std::vector<Miss> could_not_ask() const;
};

// Checks every term of every submission against the service through
// `resolver`, which keeps what it learns: a term is asked for once however
// many records and samples carry it, and an upload given the same resolver
// asks for nothing again. A name whose id is already set, or that is empty,
// is not checked.
PreflightReport preflight(const std::vector<Submission>& submissions, LookupResolver& resolver);

}  // namespace pychron::earthbank
