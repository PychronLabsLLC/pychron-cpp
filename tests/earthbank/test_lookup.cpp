#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "fake_earthbank.hpp"
#include "pychron/core/error.hpp"
#include "pychron/earthbank/lookup.hpp"
#include "pychron/earthbank/session.hpp"
#include "pychron/earthbank/vocab.hpp"
#include "session_support.hpp"

namespace pychron::earthbank {
namespace {

using nlohmann::json;
using testing::FakeEarthBank;
using testing::LoggedRequest;

constexpr std::string_view kMaterials = "/api/core/materials";
constexpr std::string_view kMonitors = "/api/arar/FluxMonitor";

// A fake, a session on it (signed in unless told not to be) and a resolver.
struct Desk {
  FakeEarthBank bank;
  Session session{bank, bank.base_url()};
  LookupResolver resolver{session};
  std::size_t logins = 0;

  explicit Desk(bool signed_in = true) {
    if (!signed_in) return;
    const auto in = session.sign_in(bank.login(), bank.password());
    if (!in) ADD_FAILURE() << to_string(in.error());
    logins = 1;
  }
  // The requests that were not the login.
  std::size_t asked() const { return bank.requests().size() - logins; }
  std::vector<LoggedRequest> log() const {
    std::vector<LoggedRequest> all = bank.requests();
    all.erase(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(logins));
    return all;
  }
};

void expect_found(const LookupResult& result, std::int64_t id, std::string_view name, const std::string& what) {
  EXPECT_EQ(result.outcome, LookupOutcome::Found) << what << ": " << result.detail;
  EXPECT_EQ(result.id, id) << what;
  EXPECT_EQ(result.matched_name, name) << what;
}

void expect_not(const LookupResult& result, LookupOutcome outcome, const std::string& what) {
  EXPECT_EQ(result.outcome, outcome) << what << ": " << result.detail;
  EXPECT_EQ(result.id, 0) << what;
  EXPECT_EQ(result.matched_name, "") << what;
  if (outcome != LookupOutcome::Found) EXPECT_FALSE(result.detail.empty()) << what;
}

// What one request asked a list: `name.equals=Sanidine size=2`.
std::string question(const LoggedRequest& request) {
  std::string out;
  for (const auto& [key, value] : request.query) out += (out.empty() ? "" : " ") + key + "=" + value;
  return out;
}

std::vector<std::string> questions(const Desk& desk, std::string_view endpoint) {
  std::vector<std::string> out;
  for (const LoggedRequest& request : desk.log()) {
    if (request.path == endpoint) out.push_back(question(request));
  }
  return out;
}

// ---- A short list, read once -----------------------------------------------

TEST(Lookup, CachedExactIsCaseInsensitive) {
  Desk desk;
  expect_found(desk.resolver.resolve("arMethodName", "step-heating - LASER"), 1303, "Step-heating - laser", "case");
  // The whole list in one request, and kept.
  EXPECT_EQ(questions(desk, "/api/arar/LArArMethod"), (std::vector<std::string>{"size=2000"}));
  expect_found(desk.resolver.resolve("arMethodName", "Total fusion - laser"), 1308, "Total fusion - laser", "exact");
  expect_found(desk.resolver.resolve("arMethodName", "  total FUSION - furnace "), 1307, "Total fusion - furnace",
               "spaces around");
  EXPECT_EQ(desk.asked(), 1u);

  // The list's own stray space is no other name (`Renne et al. 2011 `).
  expect_found(desk.resolver.resolve("decayConstantName", "Renne et al. 2011"), 1703, "Renne et al. 2011", "trimmed");
  expect_found(desk.resolver.resolve("decayConstantName", "Renne et al. 2011 "), 1703, "Renne et al. 2011", "as listed");
  // Four fields are of one list: it is read once for all of them.
  for (const char* field : {"analyticalUncertaintyUnitName", "apparentAgeUncertaintyUnitName",
                            "jvalueUncertaintyUnitName", "calculatedAgeUncertaintyTypeUnitsName"}) {
    expect_found(desk.resolver.resolve(field, "abs."), 1202, "abs.", field);
    expect_found(desk.resolver.resolve(field, "%"), 1201, "%", field);
  }
  EXPECT_EQ(desk.bank.count("GET", "/api/arar/LUncertaintyUnit"), 1u);
  EXPECT_EQ(desk.asked(), 3u);
}

// One that could be either of two entries is neither: nothing is guessed, and
// nothing more is asked that might pick one.
TEST(Lookup, AnAmbiguousNameIsUnknown) {
  Desk desk;
  desk.bank.add_lookup_row("/api/core/l-sample-methods", {{"id", 2290}, {"name", "Vial"}});
  expect_found(desk.resolver.resolve("sampleMethodName", "vial"), 2209, "vial", "the one written so");
  expect_found(desk.resolver.resolve("sampleMethodName", "Vial"), 2290, "Vial", "the other");
  expect_not(desk.resolver.resolve("sampleMethodName", "VIAL"), LookupOutcome::Unknown, "either");
  EXPECT_EQ(desk.asked(), 1u);

  // Two entries that differ in case stay two, though one of them be there
  // twice.
  desk.bank.add_lookup_row("/api/core/l-sample-methods", {{"id", 2291}, {"name", "Vial"}});
  LookupResolver again(desk.session);
  expect_not(again.resolve("sampleMethodName", "VIAL"), LookupOutcome::Unknown, "either, still");
  expect_found(desk.resolver.resolve("archiveName", "geological survey of victoria"), *desk.bank.lookup_id(
                   "/api/core/archives", "Geological Survey of Victoria"),
               "Geological Survey of Victoria", "another of that list");
  EXPECT_EQ(desk.bank.count("GET", "/api/core/archives"), 1u);
}

// The archives hold `Unknown` twice, under two ids (the template's list
// does). One spelling under several ids is one entry listed more than once:
// the lowest id is taken (legacy took the first row), and the result says so.
TEST(Lookup, OneSpellingUnderSeveralIdsIsTheLowest) {
  Desk desk;
  const LookupResult twice = desk.resolver.resolve("archiveName", "unknown");
  expect_found(twice, 2401, "Unknown", "listed twice");
  EXPECT_EQ(twice.detail, "EarthBank's list has 'Unknown' 2 times, under the ids 2401 and 2419; the lowest was taken.");
  // Whatever the order of the rows, and with the spaces a row may have.
  desk.bank.add_lookup_row("/api/arar/AirRatio", {{"id", 1890}, {"name", "Example 2031"}});
  desk.bank.add_lookup_row("/api/arar/AirRatio", {{"id", 1870}, {"name", " Example 2031 "}});
  desk.bank.add_lookup_row("/api/arar/AirRatio", {{"id", 1880}, {"name", "Example 2031"}});
  const LookupResult thrice = desk.resolver.resolve("airRatioName", "Example 2031");
  expect_found(thrice, 1870, "Example 2031", "three rows");
  EXPECT_EQ(thrice.detail,
            "EarthBank's list has 'Example 2031' 3 times, under the ids 1870, 1880 and 1890; the lowest was taken.");
  // An entry there once has nothing said of it.
  EXPECT_EQ(desk.resolver.resolve("archiveName", "Geological Survey of Victoria").detail, "");
  // A monitor found by its own rule, likewise.
  desk.bank.add_lookup_row(kMonitors, {{"id", 1990}, {"name", "Fish Canyon Tuff Sanidine (FCTs) from Kuiper et al. 2008 (28.201 Ma)"}});
  const LookupResult monitor = desk.resolver.resolve("fluxMonitorName", "FC-2 28.201 Ma");
  expect_found(monitor, 1903, "Fish Canyon Tuff Sanidine (FCTs) from Kuiper et al. 2008 (28.201 Ma)", "a monitor twice");
  EXPECT_NE(monitor.detail.find("under the ids 1903 and 1990"), std::string::npos) << monitor.detail;

  // Only of a list read whole, where every row was seen: of two rows that
  // came back to a question by name, which is the lowest of all is not known
  // (ContainsNeedsAnExactMatchAmongTheResults).
}

// A list that comes back with as many rows as were asked for may have been
// cut there: it is not taken for the whole list, and its terms are asked for
// by name.
TEST(Lookup, AListAsLongAsWasAskedForIsNotTakenAsWhole) {
  Desk desk;
  json rows = json::array();
  // The first 2000 rows of a longer list, as far as can be told: the term
  // wanted is not among them, another is there under two ids, and a monitor
  // that would be the only one of its age.
  rows.push_back({{"id", 5}, {"name", "Twice"}});
  rows.push_back({{"id", 6}, {"name", "Twice"}});
  while (rows.size() < 2000) rows.push_back({{"id", 10000 + rows.size()}, {"name", "Filler " + std::to_string(rows.size())}});
  desk.bank.fail_next("GET", "/api/arar/LAgeType", 200, rows.dump());
  expect_found(desk.resolver.resolve("ageCalcTypeName", "plateau age"), 1401, "Plateau Age", "by name");
  expect_not(desk.resolver.resolve("ageCalcTypeName", "Twice"), LookupOutcome::Unknown, "not the lowest of what was seen");
  EXPECT_EQ(questions(desk, "/api/arar/LAgeType"),
            (std::vector<std::string>{"size=2000", "name.equals=plateau age size=2", "name.equals=Plateau age size=2",
                                      "name.contains=plateau age size=200", "name.equals=Twice size=2",
                                      "name.contains=Twice size=200"}));

  json monitors = json::array();
  monitors.push_back({{"id", 7}, {"name", "Fish Canyon Tuff Sanidine (FCTs) from Example et al. 2031 (28.201 Ma)"}});
  while (monitors.size() < 2000) {
    monitors.push_back({{"id", 20000 + monitors.size()}, {"name", "Filler " + std::to_string(monitors.size())}});
  }
  desk.bank.fail_next("GET", kMonitors, 200, monitors.dump());
  expect_not(desk.resolver.resolve("fluxMonitorName", "FC-2 28.201 Ma"), LookupOutcome::Unknown,
             "the only entry of that age among the rows seen");
  // One row fewer, and it is the list.
  Desk whole;
  monitors.erase(monitors.size() - 1);
  whole.bank.fail_next("GET", kMonitors, 200, monitors.dump());
  expect_found(whole.resolver.resolve("fluxMonitorName", "FC-2 28.201 Ma"), 7,
               "Fish Canyon Tuff Sanidine (FCTs) from Example et al. 2031 (28.201 Ma)", "a list that ended");
}

// A name the short list has not is still asked for by name, as legacy did: a
// list cut short by the service must not make a term unknown.
TEST(Lookup, ANameNotInTheListReadIsAskedFor) {
  Desk desk;
  expect_not(desk.resolver.resolve("arMethodName", "Step-heating - oven"), LookupOutcome::Unknown, "no such method");
  EXPECT_EQ(questions(desk, "/api/arar/LArArMethod"),
            (std::vector<std::string>{"size=2000", "name.equals=Step-heating - oven size=2",
                                      "name.contains=Step-heating - oven size=200"}));
  // A list of which only the first rows came: the entry is found by name.
  Desk cut;
  cut.bank.fail_next("GET", "/api/arar/LAgeType", 200, R"([{"id":1401,"name":"Plateau Age"}])");
  expect_found(cut.resolver.resolve("ageCalcTypeName", "Total Gas Age"), 1407, "Total Gas Age", "beyond the cut");
  expect_found(cut.resolver.resolve("ageCalcTypeName", "Plateau Age"), 1401, "Plateau Age", "within it");
  EXPECT_EQ(cut.bank.count("GET", "/api/arar/LAgeType"), 2u);
}

// ---- A long list, asked by name --------------------------------------------

TEST(Lookup, EqualsThenCapitalizedThenContains) {
  Desk desk;
  // As the list has it: one question.
  expect_found(desk.resolver.resolve("materialName", "Sanidine"), 38433, "Sanidine", "exact");
  EXPECT_EQ(questions(desk, kMaterials), (std::vector<std::string>{"name.equals=Sanidine size=2"}));

  // In lower case: the service's `equals` minds the case, so the name is
  // asked again as the service capitalises its entries.
  expect_found(desk.resolver.resolve("mineralName", "sanidine"), 38433, "Sanidine", "capitalised");
  EXPECT_EQ(questions(desk, kMaterials),
            (std::vector<std::string>{"name.equals=Sanidine size=2", "name.equals=sanidine size=2"}))
      << "the capitalised form had been asked already";

  // A capital inside: neither form is the entry, and the entries holding the
  // text are read for the one that is the name but for its case.
  expect_found(desk.resolver.resolve("materialName", "alkali feldspar"), 30191, "Alkali Feldspar", "contains");
  const std::vector<std::string> asked = questions(desk, kMaterials);
  ASSERT_EQ(asked.size(), 5u);
  EXPECT_EQ(asked[2], "name.equals=alkali feldspar size=2");
  EXPECT_EQ(asked[3], "name.equals=Alkali feldspar size=2");
  EXPECT_EQ(asked[4], "name.contains=alkali feldspar size=200");

  // Not there at all: the three questions, and unknown.
  expect_not(desk.resolver.resolve("materialName", "Unobtainium"), LookupOutcome::Unknown, "no such material");
  EXPECT_EQ(questions(desk, kMaterials).size(), 7u) << "its capitalised form is itself";
  expect_not(desk.resolver.resolve("materialName", "unobtainium ore"), LookupOutcome::Unknown, "no such material");
  EXPECT_EQ(questions(desk, kMaterials).size(), 10u);
  // Nothing to ask for.
  expect_not(desk.resolver.resolve("materialName", ""), LookupOutcome::Unknown, "empty");
  expect_not(desk.resolver.resolve("materialName", "   "), LookupOutcome::Unknown, "blank");
  EXPECT_EQ(questions(desk, kMaterials).size(), 10u);
}

// The materials are some fifty thousand rows on the real service: the list
// is never read whole, whatever is asked of it, and nor is any other list the
// table does not mark as short.
TEST(Lookup, ALongListIsOnlyEverAskedForAName) {
  Desk desk;
  std::set<std::string> long_lists;
  for (const LookupSpec& spec : lookup_specs()) {
    if (spec.endpoint.empty() || spec.whole) continue;
    long_lists.insert(std::string(spec.endpoint));
    for (const char* value : {"Sanidine", "biotite", "Unobtainium", "x", "Example Creek Volcanics", "A & B"}) {
      (void)desk.resolver.resolve(spec.name_field, value);
    }
  }
  EXPECT_TRUE(long_lists.contains(std::string(kMaterials)));
  ASSERT_EQ(long_lists.size(), 4u);
  std::size_t seen = 0;
  for (const LoggedRequest& request : desk.log()) {
    ASSERT_TRUE(long_lists.contains(request.path)) << request;
    ++seen;
    const bool by_name = request.param("name.equals").has_value() || request.param("name.contains").has_value();
    EXPECT_TRUE(by_name) << request;
    const std::optional<std::string> size = request.param("size");
    ASSERT_TRUE(size.has_value()) << request;
    EXPECT_LE(std::stoi(*size), 200) << request;
    EXPECT_FALSE(request.param("page").has_value()) << request;
  }
  EXPECT_GT(seen, 20u);
}

TEST(Lookup, ContainsNeedsAnExactMatchAmongTheResults) {
  Desk desk;
  // Three entries hold the text and none is it.
  expect_not(desk.resolver.resolve("materialName", "sanidin"), LookupOutcome::Unknown, "a part of three names");
  EXPECT_EQ(questions(desk, kMaterials).back(), "name.contains=sanidin size=200");
  // More than a page of entries hold `biotit`; none is it.
  expect_not(desk.resolver.resolve("materialName", "biotit"), LookupOutcome::Unknown, "a part of many");

  // Two entries that are the name but for case: neither is chosen.
  desk.bank.add_lookup_row(kMaterials, {{"id", 39001}, {"name", "Example Mica"}});
  desk.bank.add_lookup_row(kMaterials, {{"id", 39002}, {"name", "example mica"}});
  expect_not(desk.resolver.resolve("materialName", "EXAMPLE MICA"), LookupOutcome::Unknown, "two spellings");
  expect_found(desk.resolver.resolve("materialName", "example mica"), 39002, "example mica", "written as one of them");
  expect_found(desk.resolver.resolve("materialName", "Example Mica"), 39001, "Example Mica", "and as the other");

  // Two rows of one name under two ids: an id is not picked.
  desk.bank.add_lookup_row(kMaterials, {{"id", 39003}, {"name", "Twin Rock"}});
  desk.bank.add_lookup_row(kMaterials, {{"id", 39004}, {"name", "Twin Rock"}});
  expect_not(desk.resolver.resolve("materialName", "Twin Rock"), LookupOutcome::Unknown, "one name, two ids");
  expect_not(desk.resolver.resolve("materialName", "twin rock"), LookupOutcome::Unknown, "likewise, by its parts");

  // A row that is not a row is passed over; an answer that is not a list is
  // no answer.
  desk.bank.fail_next("GET", kMaterials, 200, R"(["x", {"name":"Odd"}, {"id":"n","name":"Odd"}, {"id":39010,"name":"Odd"}])");
  expect_found(desk.resolver.resolve("materialName", "Odd"), 39010, "Odd", "the one row with an id and a name");
  desk.bank.fail_next("GET", kMaterials, 200, R"({"content":[]})");
  expect_not(desk.resolver.resolve("materialName", "Paged"), LookupOutcome::CouldNotAsk, "not a list");
  desk.bank.fail_next("GET", kMaterials, 200, "<html>");
  expect_not(desk.resolver.resolve("materialName", "Markup"), LookupOutcome::CouldNotAsk, "not JSON");
}

// ---- The flux monitor ------------------------------------------------------

TEST(Lookup, FluxMonitorFuzzyByAuthorAndYear) {
  Desk desk;
  const char* const phillips = "Alder Creek Rhyolite Sanidine (ACs) from Phillips et al. 2022 (1.1834 Ma)";
  const char* const kuiper = "Fish Canyon Tuff Sanidine (FCTs) from Kuiper et al. 2008 (28.201 Ma)";
  // The entry legacy quotes, from the query it composed: the monitor's name
  // and its reference.
  expect_found(desk.resolver.resolve("fluxMonitorName", "Alder Creek Rhyolite Sanidine Phillips et al. 2022"), 1901,
               phillips, "legacy's");
  // In the order of the spec: the list (read once), then by name, then the
  // monitor's own rule on the list that was read.
  EXPECT_EQ(questions(desk, kMonitors),
            (std::vector<std::string>{"size=2000",
                                      "name.equals=Alder Creek Rhyolite Sanidine Phillips et al. 2022 size=2",
                                      "name.equals=Alder creek rhyolite sanidine phillips et al. 2022 size=2",
                                      "name.contains=Alder Creek Rhyolite Sanidine Phillips et al. 2022 size=200"}));
  // What the mapping writes: pychron's name for the monitor and its age.
  expect_found(desk.resolver.resolve("fluxMonitorName", flux_monitor_query("FC-2", 28.201)), 1903, kuiper, "by age");
  expect_found(desk.resolver.resolve("fluxMonitorName", "FC-2 Kuiper et al. 2008"), 1903, kuiper, "by paper");
  expect_found(desk.resolver.resolve("fluxMonitorName", kuiper), 1903, kuiper, "the entry itself");
  EXPECT_EQ(desk.bank.count("GET", kMonitors), 10u) << "the list, and three questions for each name not in it";

  // Where legacy took the first entry holding the first word and the year,
  // nothing is taken: a wrong monitor on a published age cannot be put right.
  expect_not(desk.resolver.resolve("fluxMonitorName", "Renne et al. 2011"), LookupOutcome::Unknown, "three monitors' paper");
  expect_not(desk.resolver.resolve("fluxMonitorName", "FC-2 Phillips 2008"), LookupOutcome::Unknown, "no such entry");
  expect_not(desk.resolver.resolve("fluxMonitorName", "Fish Canyon biotite 28.201 Ma"), LookupOutcome::Unknown,
             "another mineral");
  expect_not(desk.resolver.resolve("fluxMonitorName", "FC-2 28.2 Ma"), LookupOutcome::Unknown, "an age cut short");
  expect_not(desk.resolver.resolve("fluxMonitorName", "Mt Domedary Biotite 99.44 Ma"), LookupOutcome::Unknown,
             "two splits of one age");

  // The rule is the monitor's alone: a decay constant is not found by its
  // author and year among the entries.
  expect_not(desk.resolver.resolve("decayConstantName", "Renne 2011"), LookupOutcome::Unknown, "not a monitor");
}

// Whether the service's rows carry more than a name is not known. Where one
// does, nothing of it finds a flux monitor: a reference is a paper, and a
// paper names no monitor; a description is free text of no known shape.
TEST(Lookup, AFluxMonitorIsNotFoundByWhatElseItsRowSays) {
  Desk desk;
  desk.bank.add_lookup_row(kMonitors, {{"id", 1950},
                                       {"name", "Example Tuff Sanidine (ETs)"},
                                       {"reference", "Sample et al. 2031"},
                                       {"description", "An invented monitor."}});
  desk.bank.add_lookup_row(kMonitors, {{"id", 1951},
                                       {"name", "Example Tuff Biotite (ETb)"},
                                       {"reference", "Other and Sample 2032"},
                                       {"description", nullptr}});
  for (const char* value : {"sample et al. 2031", "Sample et al. 2031", "Example Tuff sanidine Sample et al. 2031",
                            "ETs Sample et al. 2031", "ETb Other and Sample 2032", "An invented monitor.",
                            "Example Tuff Sanidine (ETs) Sample et al. 2031 An invented monitor."}) {
    expect_not(desk.resolver.resolve("fluxMonitorName", value), LookupOutcome::Unknown, value);
  }
  // By its name it is found as any entry is, and the others as before.
  expect_found(desk.resolver.resolve("fluxMonitorName", "example tuff sanidine (ets)"), 1950,
               "Example Tuff Sanidine (ETs)", "its name");
  expect_found(desk.resolver.resolve("fluxMonitorName", "FC-2 28.201 Ma"), 1903,
               "Fish Canyon Tuff Sanidine (FCTs) from Kuiper et al. 2008 (28.201 Ma)", "no reference");
  const std::vector<std::string> asked = questions(desk, kMonitors);
  EXPECT_EQ(std::count(asked.begin(), asked.end(), "size=2000"), 1) << "the list is read once";
}

// On the other short lists a value that is exactly one row's `reference` is
// that row (legacy read it so); a description is never read.
TEST(Lookup, AReferenceThatIsOneRowsIsThatRow) {
  Desk desk;
  const char* const list = "/api/arar/DecayConstant";
  desk.bank.add_lookup_row(list, {{"id", 1750}, {"name", "Example and Sample 2031"}, {"reference", "EX-31"},
                                  {"description", "an invented constant"}});
  desk.bank.add_lookup_row(list, {{"id", 1751}, {"name", "Example and Sample 2032"}, {"reference", "EX-32"}});
  desk.bank.add_lookup_row(list, {{"id", 1752}, {"name", "Example and Sample 2033"}, {"reference", "Shared"}});
  desk.bank.add_lookup_row(list, {{"id", 1753}, {"name", "Example and Sample 2034"}, {"reference", "Shared"},
                                  {"description", "EX-31"}});
  const LookupResult by_reference = desk.resolver.resolve("decayConstantName", "ex-31");
  expect_found(by_reference, 1750, "Example and Sample 2031", "one row's");
  EXPECT_EQ(by_reference.detail, "Found by the reference of the entry, 'EX-31', not by its name.");
  expect_found(desk.resolver.resolve("decayConstantName", "EX-32"), 1751, "Example and Sample 2032", "the other's");
  expect_not(desk.resolver.resolve("decayConstantName", "Shared"), LookupOutcome::Unknown, "two rows' reference");
  expect_not(desk.resolver.resolve("decayConstantName", "EX-3"), LookupOutcome::Unknown, "a part of one");
  expect_not(desk.resolver.resolve("decayConstantName", "an invented constant"), LookupOutcome::Unknown,
             "a description");
  // A name comes first.
  expect_found(desk.resolver.resolve("decayConstantName", "Example and Sample 2033"), 1752, "Example and Sample 2033",
               "by name");
}

// ---- What is not asked -----------------------------------------------------

TEST(Lookup, UncertaintyTypeComesFromTheStaticTable) {
  Desk desk(false);
  for (const LookupSpec& spec : uncertainty_type_specs()) {
    expect_found(desk.resolver.resolve(spec.name_field, "1 sigma"), 1, "1 sigma", std::string(spec.name_field));
    expect_found(desk.resolver.resolve(spec.name_field, "2s"), 2, "2 sigma", std::string(spec.name_field));
    expect_found(desk.resolver.resolve(spec.name_field, "MSEM"), 1, "1 sigma", std::string(spec.name_field));
    // The list that looks like theirs holds these; a record refuses its ids.
    expect_not(desk.resolver.resolve(spec.name_field, "95% confidence"), LookupOutcome::Unknown, "no such level");
    expect_not(desk.resolver.resolve(spec.name_field, ""), LookupOutcome::Unknown, "empty");
  }
  EXPECT_TRUE(desk.bank.requests().empty());
}

TEST(Lookup, AFieldWithNoListIsNotAsked) {
  Desk desk;
  // Sent by name: the server keeps the three as an enumeration.
  for (const char* field : {"plateauStepName", "inverseIsoStepName", "weightedMeanStepName"}) {
    expect_not(desk.resolver.resolve(field, "Yes"), LookupOutcome::NoList, field);
  }
  // Not terms at all.
  for (const char* field : {"lithologyName", "aliquotName", "locationName", "name", "mineralId", ""}) {
    expect_not(desk.resolver.resolve(field, "Basalt"), LookupOutcome::NoList, field);
  }
  EXPECT_EQ(desk.asked(), 0u);
}

// ---- A list that cannot be asked -------------------------------------------

TEST(Lookup, CouldNotAskWithoutALogin) {
  Desk desk(false);
  // A list that wants a login, and none to give: not "unknown".
  const LookupResult closed = desk.resolver.resolve("sampleKindName", "Ore");
  expect_not(closed, LookupOutcome::CouldNotAsk, "a core list");
  EXPECT_EQ(closed.detail, "EarthBank wants a login for this list.");
  // An open list answers without one.
  expect_found(desk.resolver.resolve("arMethodName", "Step-heating - laser"), 1303, "Step-heating - laser", "open");
  expect_found(desk.resolver.resolve("materialName", "Sanidine"), 38433, "Sanidine", "open, by name");

  // Which lists want one is not known beyond the fake's guess: a 401 from
  // any of them is the same answer, on a list read whole and on one asked by
  // name.
  desk.bank.fail_next("GET", "/api/arar/AirRatio", 401, R"({"title":"Unauthorized"})");
  expect_not(desk.resolver.resolve("airRatioName", "Nier 1950"), LookupOutcome::CouldNotAsk, "an Ar-Ar list, closed");
  desk.bank.fail_next("GET", "/api/core/literature", 403, "");
  expect_not(desk.resolver.resolve("literatureName", "Sample 2025, Fixture Letters 3, 45-50"),
             LookupOutcome::CouldNotAsk, "forbidden");
  EXPECT_EQ(desk.bank.count("GET", "/api/core/literature"), 1u) << "the other forms are not tried";

  // And it is asked once: every later term of that list is the same.
  expect_not(desk.resolver.resolve("sampleKindName", "Xenolith"), LookupOutcome::CouldNotAsk, "the list again");
  expect_not(desk.resolver.resolve("airRatioName", "Lee et al. 2006"), LookupOutcome::CouldNotAsk, "likewise");
  expect_not(desk.resolver.resolve("literatureName", "Sample 2025, Fixture Letters 3, 45-50"),
             LookupOutcome::CouldNotAsk, "the same name again");
  EXPECT_EQ(desk.bank.count("GET", "/api/core/l-sample-kinds"), 1u);
  EXPECT_EQ(desk.bank.count("GET", "/api/arar/AirRatio"), 1u);
  EXPECT_EQ(desk.bank.count("GET", "/api/core/literature"), 1u);
  EXPECT_EQ(desk.bank.count("POST", "/api/authenticate"), 0u);

  // With a login the same list answers.
  Desk in;
  expect_found(in.resolver.resolve("sampleKindName", "ore"), 2101, "Ore", "signed in");
}

// No answer, or a fault, is "could not ask" too, and says which: the term is
// not known to be unknown.
TEST(Lookup, AListThatDoesNotAnswerCouldNotBeAsked) {
  Desk desk;
  desk.bank.lose_next("GET", "/api/arar/DecayConstant");
  const LookupResult silent = desk.resolver.resolve("decayConstantName", "Min et al. 2000");
  expect_not(silent, LookupOutcome::CouldNotAsk, "a timeout");
  EXPECT_EQ(silent.detail, "EarthBank did not answer (list of terms). Check the network and try again.");

  desk.bank.fail_next("GET", "/api/arar/LArArInterpretation", 500, R"({"title":"Internal Server Error"})");
  const LookupResult fault = desk.resolver.resolve("interpretationName", "Unknown");
  expect_not(fault, LookupOutcome::CouldNotAsk, "a 500");
  EXPECT_EQ(fault.detail, "EarthBank failed on its own side at the list of terms (HTTP 500). Try again later; if it "
                          "goes on, tell EarthBank's support.");

  desk.bank.refuse_next("GET", kMaterials);
  expect_not(desk.resolver.resolve("materialName", "Sanidine"), LookupOutcome::CouldNotAsk, "unreachable");
  desk.bank.fail_next("GET", kMaterials, 404, "");
  const LookupResult nowhere = desk.resolver.resolve("mineralName", "Biotite");
  expect_not(nowhere, LookupOutcome::CouldNotAsk, "a wrong address");
  EXPECT_NE(nowhere.detail.find("base URL"), std::string::npos) << nowhere.detail;

  // The token gone bad in between is the session's to mend, not a miss.
  desk.bank.expire_tokens();
  expect_found(desk.resolver.resolve("sampleKindName", "Ore"), 2101, "Ore", "after a new login");
  EXPECT_EQ(desk.bank.count("POST", "/api/authenticate"), 2u);
  EXPECT_TRUE(desk.bank.pending().empty());
}

// What the service says of a list it will not give goes into `detail`, and
// may say a secret back: none is left, whole or in part.
TEST(Lookup, NoSecretInADetail) {
  FakeEarthBank::Options options;
  options.password = "Zq7pre;Zq7post";
  FakeEarthBank bank(options);
  testing::Spy spy(bank);
  Session session(spy, bank.base_url());
  ASSERT_TRUE(session.sign_in(bank.login(), options.password));
  LookupResolver resolver(session);
  const auto echo = [&](std::string_view list, int status, const char* key) {
    bank.fail_next("GET", list, status,
                   json{{key, "token " + spy.token() + " and password " + options.password + "; more"}}.dump());
  };
  std::vector<std::string> details;
  echo("/api/arar/AirRatio", 400, "detail");
  details.push_back(resolver.resolve("airRatioName", "Nier 1950").detail);
  echo("/api/arar/DecayConstant", 403, "message");
  details.push_back(resolver.resolve("decayConstantName", "Min et al. 2000").detail);
  echo(kMaterials, 409, "title");
  details.push_back(resolver.resolve("materialName", "Sanidine").detail);
  echo("/api/core/literature", 500, "detail");
  details.push_back(resolver.resolve("literatureName", "A paper").detail);
  EXPECT_TRUE(bank.pending().empty());
  EXPECT_EQ(details[0], "EarthBank refused the list of terms: token <redacted> and password <redacted>");
  for (const std::string& detail : details) {
    EXPECT_FALSE(detail.empty());
    EXPECT_EQ(detail.find("Zq7"), std::string::npos) << detail;
    for (const std::string& secret : spy.secrets()) EXPECT_EQ(detail.find(secret), std::string::npos) << detail;
  }
}

// ---- Once ------------------------------------------------------------------

TEST(Lookup, AsksOncePerValue) {
  Desk desk;
  for (int i = 0; i < 50; ++i) {
    expect_found(desk.resolver.resolve("materialName", "Sanidine"), 38433, "Sanidine", "found");
    expect_found(desk.resolver.resolve("mineralName", " Sanidine "), 38433, "Sanidine", "the other field of the list");
    expect_not(desk.resolver.resolve("materialName", "Unobtainium"), LookupOutcome::Unknown, "not found");
    expect_found(desk.resolver.resolve("fluxMonitorName", "FC-2 28.201 Ma"), 1903,
                 "Fish Canyon Tuff Sanidine (FCTs) from Kuiper et al. 2008 (28.201 Ma)", "a monitor");
    expect_found(desk.resolver.resolve("airRatioName", "Nier 1950"), 1802, "Nier 1950", "a short list");
    expect_found(desk.resolver.resolve("airRatioName", "Lee et al. 2006"), 1801, "Lee et al. 2006", "the same list");
  }
  // Sanidine: one question. Unobtainium: two (its capitalised form is
  // itself). The monitor: the list, and three by name. The air ratios: the
  // list.
  EXPECT_EQ(desk.bank.count("GET", kMaterials), 3u);
  EXPECT_EQ(desk.bank.count("GET", kMonitors), 4u);
  EXPECT_EQ(desk.bank.count("GET", "/api/arar/AirRatio"), 1u);
  EXPECT_EQ(desk.asked(), 8u);

  // Another resolver asks afresh: what is kept is this one's.
  LookupResolver other(desk.session);
  expect_found(other.resolve("materialName", "Sanidine"), 38433, "Sanidine", "a new resolver");
  EXPECT_EQ(desk.asked(), 9u);
}

// ---- What goes into a URL --------------------------------------------------

TEST(Lookup, ValuesAreEncoded) {
  Desk desk;
  const std::string cafe = "Caf\xC3\xA9 & Co";
  desk.bank.add_lookup_row(kMaterials, {{"id", 39100}, {"name", cafe}});
  desk.bank.add_lookup_row(kMaterials, {{"id", 39101}, {"name", "50% Quartz=Feldspar? #2 <x> \"y\" a/b"}});
  expect_found(desk.resolver.resolve("materialName", cafe), 39100, cafe, "an accent and an ampersand");
  const LoggedRequest asked = desk.log().back();
  EXPECT_EQ(asked.status, 200);
  EXPECT_EQ(asked.param("name.equals"), cafe);
  EXPECT_EQ(asked.param("size"), "2");
  EXPECT_EQ(asked.query.size(), 2u) << "the ampersand made no parameter of its own";

  expect_found(desk.resolver.resolve("materialName", "50% Quartz=Feldspar? #2 <x> \"y\" a/b"), 39101,
               "50% Quartz=Feldspar? #2 <x> \"y\" a/b", "what a query is made of");
  // Entries of the vendor's list: a plus that must not arrive as a space,
  // parentheses, an apostrophe.
  expect_found(desk.resolver.resolve("materialName", "Julgoldite-(Fe2+)-Pumpellyite-(Mg) Series"), 34737,
               "Julgoldite-(Fe2+)-Pumpellyite-(Mg) Series", "a plus");
  expect_found(desk.resolver.resolve("materialName", "arizonite (of spurr & washington)"), 30712,
               "Arizonite (of Spurr & Washington)", "by its parts");
  // In lower case the accented letter is left as it is: capitalising is of
  // ASCII letters only, and the name is found among those that hold it.
  expect_found(desk.resolver.resolve("materialName", "caf\xC3\xA9 & co"), 39100, cafe, "contains");
  // Every request was one the service could read.
  for (const LoggedRequest& request : desk.log()) EXPECT_EQ(request.status, 200) << request;
}

}  // namespace
}  // namespace pychron::earthbank
