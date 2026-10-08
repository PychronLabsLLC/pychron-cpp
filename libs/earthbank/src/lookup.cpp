#include "pychron/earthbank/lookup.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "pychron/core/error.hpp"
#include "pychron/earthbank/http.hpp"
#include "pychron/earthbank/vocab.hpp"

namespace pychron::earthbank {
namespace {

using nlohmann::json;

// Legacy's three sizes: a whole list, an exact name, the names holding a
// text. An exact name is asked for two rows, not legacy's one, so that two
// rows of one name are seen to be two.
constexpr int kWholeList = 2000;
constexpr int kExactRows = 2;
constexpr int kContainingRows = 200;

constexpr std::string_view kFluxMonitorField = "fluxMonitorName";

bool is_blank(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

std::string_view trimmed(std::string_view text) {
  while (!text.empty() && is_blank(text.front())) text.remove_prefix(1);
  while (!text.empty() && is_blank(text.back())) text.remove_suffix(1);
  return text;
}

// The first letter in capitals and the rest in lower case, of ASCII letters:
// how the service spells most of its entries (legacy's second try).
std::string capitalised(std::string_view text) {
  std::string out(text);
  for (std::size_t i = 0; i < out.size(); ++i) {
    char& c = out[i];
    if (i == 0 && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if (i > 0 && c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

std::string text_of(const json& row, const char* key) {
  const auto it = row.find(key);
  return it != row.end() && it->is_string() ? it->get<std::string>() : std::string();
}

LookupResult unknown(std::string detail) { return {LookupOutcome::Unknown, 0, {}, std::move(detail)}; }
LookupResult could_not_ask(std::string detail) { return {LookupOutcome::CouldNotAsk, 0, {}, std::move(detail)}; }
LookupResult no_list(std::string detail) { return {LookupOutcome::NoList, 0, {}, std::move(detail)}; }

constexpr std::string_view kNotOnTheList = "EarthBank's list has no such entry.";
constexpr std::string_view kTwoEntries = "More than one entry of EarthBank's list could be meant; none was chosen.";

// How a value stands to some rows, each known by a label (its name, or its
// reference): the one row a label of which is the value, none, or more than
// one.
struct Pick {
  enum class Kind { One, None, Several };
  Kind kind = Kind::None;
  std::size_t row = 0;
};

// `match` is match_list_entry or match_flux_monitor. `labels[i]` is a label
// of the row `owners[i]`, and `ids` the rows' ids: two rows of one id are one.
template <typename Match>
Pick pick(std::string_view value, const std::vector<std::string>& labels, const std::vector<std::size_t>& owners,
          const std::vector<std::int64_t>& ids, Match match) {
  const ListMatch matched = match(value, std::span<const std::string>(labels));
  if (matched.kind == ListMatch::Kind::Ambiguous) return {Pick::Kind::Several, 0};
  if (matched.kind == ListMatch::Kind::None) return {};
  // The rows that carry the entry found: one label may be two rows'.
  std::optional<std::size_t> row;
  for (std::size_t i = 0; i < labels.size(); ++i) {
    if (trimmed(labels[i]) != trimmed(matched.entry)) continue;
    if (row && ids[*row] != ids[owners[i]]) return {Pick::Kind::Several, 0};
    if (!row) row = owners[i];
  }
  return row ? Pick{Pick::Kind::One, *row} : Pick{};
}

}  // namespace

LookupResolver::LookupResolver(Session& session) : session_(session) {}

// One GET of a list: `key` and `value` the question (`name.equals`), or no
// key for the list as it comes.
LookupResolver::Rows LookupResolver::ask(std::string_view endpoint, std::string_view key, std::string_view value,
                                         int size) {
  HttpRequest request;
  request.method = "GET";
  request.url = key.empty() ? with_query(session_.url(endpoint), {{"size", std::to_string(size)}})
                            : with_query(session_.url(endpoint), {{key, std::string(value)}, {"size", std::to_string(size)}});
  // Some lists are open and some want a login; which, beyond `/api/core/l-*`,
  // is not known. So every list is asked with the login when there is one and
  // without when there is none, and a 401 is taken as it comes.
  const Result<HttpResponse> answer = session_.send(std::move(request), false);
  Rows out;
  if (!answer) {
    out.why = session_.failure(answer.error(), "GET", endpoint, "").what;
    return out;
  }
  if (answer->status < 200 || answer->status >= 300) {
    const bool wants_login = (answer->status == 401 || answer->status == 403) && !session_.signed_in();
    out.why = wants_login ? "EarthBank wants a login for this list."
                          : session_.failure(*answer, "GET", endpoint, "").what;
    return out;
  }
  const json rows = json::parse(answer->body, nullptr, false);
  if (!rows.is_array()) {
    out.why = "EarthBank's answer was not a list of terms.";
    return out;
  }
  out.answered = true;
  for (const json& row : rows) {
    // A row is an id and a name; one without is passed over. Legacy also
    // reads a reference where a row has one (and a description, which is not
    // read here).
    if (!row.is_object()) continue;
    const auto id = row.find("id");
    if (id == row.end() || !id->is_number_integer()) continue;
    Row one{id->get<std::int64_t>(), text_of(row, "name"), text_of(row, "reference")};
    if (trimmed(one.name).empty()) continue;
    out.rows.push_back(std::move(one));
  }
  return out;
}

const LookupResolver::Rows& LookupResolver::whole(std::string_view endpoint) {
  auto it = lists_.find(endpoint);
  if (it == lists_.end()) it = lists_.emplace(std::string(endpoint), ask(endpoint, {}, {}, kWholeList)).first;
  return it->second;
}

const LookupResolver::Rows& LookupResolver::named(std::string_view endpoint, std::string_view value) {
  std::pair<std::string, std::string> key{std::string(endpoint), std::string(value)};
  auto it = equals_.find(key);
  if (it == equals_.end()) it = equals_.emplace(std::move(key), ask(endpoint, "name.equals", value, kExactRows)).first;
  return it->second;
}

LookupResult LookupResolver::find(std::string_view endpoint, bool whole_list, bool monitor, std::string_view value) {
  const auto exactly = [](std::string_view v, std::span<const std::string> entries) { return match_list_entry(v, entries); };
  const auto as_monitor = [](std::string_view v, std::span<const std::string> entries) {
    return match_flux_monitor(v, entries);
  };
  // The value among the names of `rows`: found, two that could be meant
  // (unknown, and the matter is settled), or nothing, for the next step.
  const auto among = [&](const std::vector<Row>& rows, auto match) -> std::optional<LookupResult> {
    std::vector<std::string> labels;
    std::vector<std::size_t> owners;
    std::vector<std::int64_t> ids;
    for (std::size_t i = 0; i < rows.size(); ++i) {
      labels.push_back(rows[i].name);
      owners.push_back(i);
      ids.push_back(rows[i].id);
    }
    const Pick picked = pick(value, labels, owners, ids, match);
    if (picked.kind == Pick::Kind::Several) return unknown(std::string(kTwoEntries));
    if (picked.kind == Pick::Kind::None) return std::nullopt;
    const Row& row = rows[picked.row];
    return LookupResult{LookupOutcome::Found, row.id, std::string(trimmed(row.name)), {}};
  };

  // 1. The list as it was read.
  const Rows* list = nullptr;
  if (whole_list) {
    list = &whole(endpoint);
    if (!list->answered) return could_not_ask(list->why);
    if (auto in_list = among(list->rows, exactly)) return *in_list;
    // A reference that is the value, and one row's. Not for a flux monitor:
    // its reference is a paper, and a paper names no monitor.
    if (!monitor) {
      std::vector<std::string> labels;
      std::vector<std::size_t> owners;
      std::vector<std::int64_t> ids;
      for (std::size_t i = 0; i < list->rows.size(); ++i) {
        ids.push_back(list->rows[i].id);
        if (trimmed(list->rows[i].reference).empty()) continue;
        labels.push_back(list->rows[i].reference);
        owners.push_back(i);
      }
      const Pick picked = pick(value, labels, owners, ids, exactly);
      if (picked.kind == Pick::Kind::Several) return unknown(std::string(kTwoEntries));
      if (picked.kind == Pick::Kind::One) {
        const Row& row = list->rows[picked.row];
        return {LookupOutcome::Found, row.id, std::string(trimmed(row.name)), {}};
      }
    }
  }

  // 2. By name: as given, capitalised, and among the names that hold it. A
  // row that comes back is the term only if its name is the value but for
  // case (legacy checks the same).
  const std::string given(value);
  const std::string capital = capitalised(value);
  for (const std::string* form : {&given, &capital}) {
    if (form == &capital && capital == given) break;
    const Rows& rows = named(endpoint, *form);
    if (!rows.answered) return could_not_ask(rows.why);
    if (auto exact = among(rows.rows, exactly)) return *exact;
  }
  const Rows holding = ask(endpoint, "name.contains", value, kContainingRows);
  if (!holding.answered) return could_not_ask(holding.why);
  if (auto held = among(holding.rows, exactly)) return *held;

  // 3. The flux monitor, by its own rule, over the names of the list that was
  // read. What else a row says (a reference, a description) is not read for
  // it: free text of no known shape must not find an id.
  if (monitor && list != nullptr) {
    if (auto by_rule = among(list->rows, as_monitor)) return *by_rule;
  }
  return unknown(std::string(kNotOnTheList));
}

LookupResult LookupResolver::resolve(std::string_view name_field, std::string_view value) {
  value = trimmed(value);
  // The sigma levels: a fixed table. Their list has no endpoint, and the one
  // that looks like it holds ids a record refuses.
  for (const LookupSpec& spec : uncertainty_type_specs()) {
    if (spec.name_field != name_field) continue;
    const std::optional<int> id = uncertainty_type_id(value);
    if (!id) return unknown("EarthBank's records take `1 sigma` or `2 sigma` here, and this is neither.");
    return {LookupOutcome::Found, *id, *id == 1 ? "1 sigma" : "2 sigma", {}};
  }
  const auto specs = lookup_specs();
  const auto spec = std::find_if(specs.begin(), specs.end(),
                                 [&](const LookupSpec& one) { return one.name_field == name_field; });
  if (spec == specs.end()) return no_list("This field is not a term EarthBank keeps a list of.");
  if (spec->endpoint.empty()) return no_list("EarthBank takes this field by its name.");
  if (value.empty()) return unknown("No name was given.");

  std::pair<std::string, std::string> key{std::string(spec->endpoint), std::string(value)};
  if (const auto kept = found_.find(key); kept != found_.end()) return kept->second;
  LookupResult result = find(spec->endpoint, spec->whole, name_field == kFluxMonitorField, value);
  found_.emplace(std::move(key), result);
  return result;
}

}  // namespace pychron::earthbank
