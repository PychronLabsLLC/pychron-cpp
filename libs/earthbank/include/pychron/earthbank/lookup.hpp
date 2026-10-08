// From the name of a controlled term to the service's id for it.
//
// A record carries a term as `<term>Name`; the REST API stores `<term>Id`,
// the row of the term in one of the service's lists (vocab.hpp says which
// list each field is of). A LookupResolver asks the service, through a
// Session, and remembers what it was told: a short list is read once, a name
// of a long one is asked for once.
//
// An id written to a published record cannot be corrected, and a term that
// is not found is only a line in the pre-flight report. So nothing here
// guesses: a name is an entry's only if it is that entry but for the case of
// its letters and the spaces around it (match_list_entry), a flux monitor is
// found by the rule the workbook route uses (match_flux_monitor), and a name
// that could be either of two entries is neither.
//
// The strategies, in order (the legacy integration's, spec 5.1):
//   1. the list as it was read, for a list short enough to read whole
//      (LookupSpec::whole): one request, `?size=2000`. Where the rows carry a
//      `reference` or a `description`, a value that is one row's is that row.
//   2. by name: `?name.equals=<value>`; the same with the first letter in
//      capitals and the rest in lower case, as the service spells most
//      entries (its `equals` minds the case); then `?name.contains=<value>
//      &size=200`, among whose rows one must be the name. This is how the
//      materials, some fifty thousand rows, are searched without reading
//      them; a long list is never read whole.
//   3. for the flux monitor only: match_flux_monitor over the list read in
//      step 1, by the monitor's name and age, or its name and paper.
// Step 2 follows a miss in step 1 because a list the service cut short must
// not make a term unknown; it does not follow an ambiguous name.
//
// No clock and no thread; one thread at a time, as the Session.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pychron/earthbank/session.hpp"

namespace pychron::earthbank {

enum class LookupOutcome {
  Found,        // `id` is the term's
  Unknown,      // the service was asked and has no such term, or has two that could be meant
  CouldNotAsk,  // the list did not answer: it wants a login there is not, or the request failed
  NoList,       // the field is not looked up: it is sent by name, or is no term at all
};

// `matched_name` is the entry as the service spells it, less the spaces
// around it (for an uncertainty type, `1 sigma` or `2 sigma`). `detail` says
// why, for everything but Found, in a sentence for the user.
struct LookupResult {
  LookupOutcome outcome = LookupOutcome::Unknown;
  std::int64_t id = 0;
  std::string matched_name;
  std::string detail;
};

class LookupResolver {
 public:
  // `session` must outlive the resolver. It need not be signed in: the lists
  // that are open are asked without a login, and the others give CouldNotAsk.
  explicit LookupResolver(Session& session);

  // The id of `value` for the field `name_field` (`mineralName`,
  // `fluxMonitorName`). An uncertainty type (uncertainty_type_specs) is
  // answered from the fixed table and never asked for. What was found, not
  // found or could not be asked is kept for the life of the resolver and not
  // asked again: after a sign-in, or to try a list again, make a new one.
  LookupResult resolve(std::string_view name_field, std::string_view value);

 private:
  struct Row {
    std::int64_t id = 0;
    std::string name, reference, description;
  };
  // An answer of the service to one question: its rows, or why there are none.
  struct Rows {
    bool answered = false;
    std::string why;  // when not answered
    std::vector<Row> rows;
  };

  Rows ask(std::string_view endpoint, std::string_view key, std::string_view value, int size);
  const Rows& whole(std::string_view endpoint);
  const Rows& named(std::string_view endpoint, std::string_view value);
  LookupResult find(std::string_view endpoint, bool whole_list, bool monitor, std::string_view value);

  Session& session_;
  std::map<std::string, Rows, std::less<>> lists_;                   // by endpoint: the list read whole
  std::map<std::pair<std::string, std::string>, Rows> equals_;       // by endpoint and exact name
  std::map<std::pair<std::string, std::string>, LookupResult> found_;  // by endpoint and value
};

}  // namespace pychron::earthbank
