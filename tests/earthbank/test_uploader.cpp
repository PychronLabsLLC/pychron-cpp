#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
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
#include "pychron/earthbank/http.hpp"
#include "pychron/earthbank/journal.hpp"
#include "pychron/earthbank/lookup.hpp"
#include "pychron/earthbank/model.hpp"
#include "pychron/earthbank/session.hpp"
#include "pychron/earthbank/uploader.hpp"
#include "session_support.hpp"
#include "upload_support.hpp"

namespace pychron::earthbank {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using testing::FakeEarthBank;
using testing::good_submission;
using testing::LoggedRequest;
using testing::Store;
using testing::TempDir;

constexpr std::string_view kHost = "earthbank.test";
constexpr std::string_view kRefusal = R"({"title":"Bad Request","detail":"no thank you"})";

bool has(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

bool any_has(const std::vector<std::string>& lines, std::string_view part) {
  return std::any_of(lines.begin(), lines.end(), [&](const std::string& line) { return has(line, part); });
}

// Passes every request to the fake after letting a test look at it.
class Hook final : public IHttpClient {
 public:
  explicit Hook(FakeEarthBank& bank) : bank_(bank) {}
  Result<HttpResponse> send(const HttpRequest& request) override {
    if (before) before(request);
    return bank_.send(request);
  }
  std::function<void(const HttpRequest&)> before;

 private:
  FakeEarthBank& bank_;
};

// Declared in the order they must outlive each other (AGENTS.md).
struct Rig {
  FakeEarthBank bank;
  Hook hook{bank};
  Session session{hook, bank.base_url()};
  LookupResolver resolver{session};
  TempDir dir;
  UploadOptions options;

  Rig() {
    const auto in = session.sign_in(bank.login(), bank.password());
    if (!in) ADD_FAILURE() << to_string(in.error());
    options.data_package_id = bank.writable_package();
    options.journal_dir = dir.path();
  }

  Result<UploadReport> run(const std::vector<Submission>& submissions) {
    return upload(submissions, session, resolver, options);
  }

  // The one sample of a one-submission upload.
  SampleReport run_one(const Submission& submission) {
    const auto report = run({submission});
    if (!report) {
      ADD_FAILURE() << to_string(report.error());
      return {};
    }
    EXPECT_EQ(report->samples.size(), 1u);
    return report->samples.empty() ? SampleReport{} : report->samples.front();
  }

  fs::path journal_path(std::string_view sample, std::optional<std::int64_t> package = std::nullopt) const {
    return journal_file(dir.path(), kHost, package.value_or(options.data_package_id), sample);
  }
  std::optional<Journal> journal(std::string_view sample) const {
    const auto loaded = load_journal(journal_path(sample));
    if (!loaded) {
      ADD_FAILURE() << to_string(loaded.error());
      return std::nullopt;
    }
    return *loaded;
  }

  // Requests that write a record, since the sign-in.
  std::size_t posts() const {
    std::size_t n = 0;
    for (const LoggedRequest& r : bank.requests()) n += r.method == "POST" && r.path != "/api/authenticate";
    return n;
  }
  std::size_t logins() const { return bank.count("POST", "/api/authenticate"); }
};

std::string dump(const Store& store) {
  json out = json::object();
  out["packages"] = store.packages;
  const auto put = [&](const char* key, const std::vector<Store::Record>& records) {
    json list = json::array();
    for (const Store::Record& record : records) list.push_back({record.id, record.package_id, record.body});
    out[key] = std::move(list);
  };
  put("samples", store.samples);
  put("data_points", store.data_points);
  put("aliquots", store.aliquots);
  put("measurements", store.measurements);
  put("age_calcs", store.age_calcs);
  put("age_summaries", store.age_summaries);
  return out.dump();
}

// What the service holds, as a set: the same records whatever order they were
// made in and whatever ids they were given.
std::string as_set(const Store& store) {
  json out = json::object();
  const auto put = [&](const char* key, const std::vector<Store::Record>& records) {
    std::vector<std::string> bodies;
    for (const Store::Record& record : records) {
      json body = record.body;
      body.erase("id");
      bodies.push_back(json{record.package_id, body}.dump());
    }
    std::sort(bodies.begin(), bodies.end());
    out[key] = bodies;
  };
  put("samples", store.samples);
  put("data_points", store.data_points);
  put("aliquots", store.aliquots);
  put("measurements", store.measurements);
  put("age_calcs", store.age_calcs);
  put("age_summaries", store.age_summaries);
  return out.dump();
}

// What a clean upload of `samples` leaves in the service.
std::string clean_dump(const std::vector<Submission>& samples) {
  Rig rig;
  const auto report = rig.run(samples);
  EXPECT_TRUE(report.has_value());
  for (const SampleReport& s : report->samples) EXPECT_EQ(s.status, SampleStatus::Complete);
  return dump(rig.bank.store());
}

// POSTs of one good submission with three measurements, in the order sent.
struct Call {
  const char* path;
  std::size_t skip;
  const char* label;
};
constexpr Call kCalls[] = {
    {"/api/core/sample-with-locations", 0, "sample"},
    {"/api/arar/ArArDataPoint", 0, "data point"},
    {"/api/arar/ArArAliquot", 0, "aliquot 20001-01"},
    {"/api/arar/ArArMeasurement", 0, "measurement 1"},
    {"/api/arar/ArArMeasurement", 1, "measurement 2"},
    {"/api/arar/ArArMeasurement", 2, "measurement 3"},
    {"/api/arar/ArArAgeCalc", 0, "age calculation"},
    {"/api/arar/ArArAgeSummary", 0, "age summary"},
};
constexpr std::size_t kPosts = std::size(kCalls);

// ---- The plain upload ------------------------------------------------------

TEST(Uploader, WritesEveryRecordInOrder) {
  Rig rig;
  const std::size_t before = rig.bank.requests().size();
  const SampleReport report = rig.run_one(good_submission("S-1"));
  EXPECT_EQ(report.status, SampleStatus::Complete) << ::testing::PrintToString(report.messages);
  EXPECT_TRUE(report.messages.empty()) << ::testing::PrintToString(report.messages);
  EXPECT_TRUE(report.missing.empty());
  EXPECT_EQ(report.written.size(), kPosts);

  // No sign-in after the one before, and the writes in the spec's order.
  EXPECT_EQ(rig.logins(), 1u);
  std::vector<std::string> writes;
  const std::vector<LoggedRequest> log = rig.bank.requests();
  for (std::size_t i = before; i < log.size(); ++i) {
    const LoggedRequest& r = log[i];
    if (r.path.starts_with("/api/core/sample-with-locations") || r.path.starts_with("/api/arar/ArAr")) {
      writes.push_back(r.method + " " + r.path);
    }
  }
  const std::vector<std::string> expected = {
      "GET /api/core/sample-with-locations", "POST /api/core/sample-with-locations", "POST /api/arar/ArArDataPoint",
      "POST /api/arar/ArArAliquot",          "POST /api/arar/ArArMeasurement",       "POST /api/arar/ArArMeasurement",
      "POST /api/arar/ArArMeasurement",      "POST /api/arar/ArArAgeCalc",           "POST /api/arar/ArArAgeSummary"};
  EXPECT_EQ(writes, expected);

  const Store store = rig.bank.store();
  ASSERT_EQ(store.samples.size(), 1u);
  ASSERT_EQ(store.data_points.size(), 1u);
  EXPECT_EQ(store.aliquots.size(), 1u);
  EXPECT_EQ(store.measurements.size(), 3u);
  EXPECT_EQ(store.age_calcs.size(), 1u);
  EXPECT_EQ(store.age_summaries.size(), 1u);
  const std::int64_t point = store.data_points[0].id;
  EXPECT_EQ(report.data_point_id, point);
  const auto points_to_it = [&](const std::vector<Store::Record>& records) {
    for (const Store::Record& r : records) EXPECT_EQ(r.body.value("arArDataPointId", 0), point) << r.body.dump();
  };
  points_to_it(store.aliquots);
  points_to_it(store.measurements);
  points_to_it(store.age_calcs);
  points_to_it(store.age_summaries);

  // And the journal has every id.
  const auto journal = rig.journal("S-1");
  ASSERT_TRUE(journal.has_value());
  EXPECT_EQ(journal->sample_id, store.samples[0].id);
  EXPECT_EQ(journal->data_point_id, point);
  EXPECT_EQ(journal->aliquots.at("20001-01"), store.aliquots[0].id);
  EXPECT_EQ(journal->measurements.size(), 3u);
  EXPECT_EQ(journal->measurements.at("0002"), store.measurements[1].id);
  EXPECT_EQ(journal->age_calc_id, store.age_calcs[0].id);
  EXPECT_EQ(journal->age_summary_id, store.age_summaries[0].id);
  EXPECT_FALSE(journal->in_doubt);
}

TEST(Uploader, DataPointGoesInTheWrapper) {
  Rig rig;
  ASSERT_EQ(rig.run_one(good_submission("S-1")).status, SampleStatus::Complete);
  const Store store = rig.bank.store();
  const json& body = store.data_points.at(0).body;
  EXPECT_EQ(body["dataPointDTO"]["dataStructure"], "ARARDATAPOINT");
  EXPECT_EQ(body["dataPointDTO"]["dataPackageId"], rig.options.data_package_id);
  EXPECT_EQ(body["dataPointDTO"]["sampleId"], store.samples.at(0).id);
  EXPECT_EQ(body["dataPointDTO"]["name"], "S-1 Sanidine");
  EXPECT_EQ(body["arArSetUpName"], "Noble gas lab");
  EXPECT_EQ(body["arArIrradiationName"], "IRR-1");
  EXPECT_EQ(body["extendingDataPointDTO"]["analysisDate"], "2026-05-20");
  EXPECT_EQ(store.data_points[0].package_id, rig.options.data_package_id);
  // The wrapper holds the Ar-Ar fields and nothing of the umbrella's.
  EXPECT_FALSE(body["extendingDataPointDTO"].contains("dataStructure"));

  // The sample goes in its own wrapper, with the package on both.
  const json& sample = store.samples.at(0).body;
  EXPECT_EQ(sample["dataPackageId"], rig.options.data_package_id);
  EXPECT_EQ(sample["sampleDTO"]["dataPackageId"], rig.options.data_package_id);
  EXPECT_EQ(sample["sampleDTO"]["name"], "S-1");
  EXPECT_EQ(sample["shortName"], "S-1");
  EXPECT_DOUBLE_EQ(sample["locationDTO"]["lat"].get<double>(), -34.9);
}

TEST(Uploader, NoSetUpNorIrradiationIsNotSent) {
  Rig rig;
  Submission s = good_submission("S-1");
  s.setup_name.clear();
  s.irradiation_name.clear();
  ASSERT_EQ(rig.run_one(s).status, SampleStatus::Complete);
  const Store store = rig.bank.store();
  const json& body = store.data_points.at(0).body;
  EXPECT_FALSE(body.contains("arArSetUpName"));
  EXPECT_FALSE(body.contains("arArIrradiationName"));
}

TEST(Uploader, SampleSearchIsScopedToThePackage) {
  for (const bool service_ignores_the_filter : {false, true}) {
    Rig rig;
    rig.bank.ignore_sample_package_filter(service_ignores_the_filter);
    const std::int64_t other = rig.bank.add_package("Another lab's package", true);
    // The same name, first in a package that is not ours.
    UploadOptions elsewhere = rig.options;
    elsewhere.data_package_id = other;
    ASSERT_TRUE(upload({good_submission("S-1")}, rig.session, rig.resolver, elsewhere).has_value());
    ASSERT_EQ(rig.bank.store().samples.size(), 1u);

    const SampleReport report = rig.run_one(good_submission("S-1"));
    EXPECT_EQ(report.status, SampleStatus::Complete);
    const Store store = rig.bank.store();
    ASSERT_EQ(store.samples.size(), 2u) << "a sample of another package is not reused";
    EXPECT_EQ(store.samples[1].package_id, rig.options.data_package_id);
    ASSERT_EQ(store.data_points.size(), 2u);
    EXPECT_EQ(store.data_points[1].body["dataPointDTO"]["sampleId"], store.samples[1].id);
    // The search carried the package.
    bool scoped = false;
    for (const LoggedRequest& r : rig.bank.requests()) {
      if (r.method == "GET" && r.path == "/api/core/sample-with-locations") {
        scoped = r.param("dataPackageId.equals") == std::to_string(rig.options.data_package_id) || scoped;
      }
    }
    EXPECT_TRUE(scoped);
  }
}

TEST(Uploader, ReusesAnExistingSampleInThePackage) {
  Rig rig;
  ASSERT_EQ(rig.run_one(good_submission("S-1")).status, SampleStatus::Complete);
  const std::int64_t sample = rig.bank.store().samples.at(0).id;
  const std::size_t sample_posts = rig.bank.count("POST", "/api/core/sample-with-locations");

  // A new journal (another machine, or the old one gone): the sample is found
  // by its name and not made again.
  rig.options.journal_dir = rig.dir.path() / "elsewhere";
  Submission changed = good_submission("S-1");
  changed.datapoint_name = "S-1 Sanidine, second";
  const SampleReport report = rig.run_one(changed);
  EXPECT_EQ(report.status, SampleStatus::Complete);
  EXPECT_EQ(rig.bank.count("POST", "/api/core/sample-with-locations"), sample_posts);
  const Store store = rig.bank.store();
  EXPECT_EQ(store.samples.size(), 1u);
  ASSERT_EQ(store.data_points.size(), 2u);
  EXPECT_EQ(store.data_points[1].body["dataPointDTO"]["sampleId"], sample);
  EXPECT_EQ(report.written.size(), kPosts - 1) << "the sample is not among what was written";
}

TEST(Uploader, TwoSamplesOfOneNameInThePackageAreNotGuessedBetween) {
  Rig rig;
  ASSERT_EQ(rig.run_one(good_submission("S-1")).status, SampleStatus::Complete);
  rig.options.journal_dir = rig.dir.path() / "second";
  ASSERT_EQ(rig.run_one(good_submission("S-1")).status, SampleStatus::Complete);  // reuses
  // A second one of that name, made by somebody else.
  json wrapper = {{"sampleDTO", {{"name", "S-1"}, {"dataPackageId", rig.options.data_package_id}}},
                  {"locationDTO", json::object()},
                  {"dataPackageId", rig.options.data_package_id}};
  HttpRequest make;
  make.method = "POST";
  make.url = rig.session.url("/api/core/sample-with-locations");
  make.headers.emplace_back("Content-Type", "application/json");
  make.body = wrapper.dump();
  ASSERT_TRUE(rig.session.send(make, true).has_value());

  rig.options.journal_dir = rig.dir.path() / "third";
  const std::size_t before = rig.posts();
  const SampleReport report = rig.run_one(good_submission("S-1"));
  EXPECT_EQ(report.status, SampleStatus::Stopped);
  EXPECT_EQ(rig.posts(), before) << "nothing is sent when the sample cannot be told";
  ASSERT_FALSE(report.messages.empty());
  EXPECT_TRUE(has(report.messages.front(), "more than one")) << report.messages.front();
}

TEST(Uploader, NamesAreResolvedToIdsAndNamesAreNotSent) {
  Rig rig;
  ASSERT_EQ(rig.run_one(good_submission("S-1")).status, SampleStatus::Complete);
  const Store store = rig.bank.store();
  const json& sample = store.samples.at(0).body["sampleDTO"];
  EXPECT_EQ(sample["materialId"], rig.bank.lookup_id("/api/core/materials", "Sanidine"));
  EXPECT_FALSE(sample.contains("materialName"));

  const json& ext = store.data_points.at(0).body["extendingDataPointDTO"];
  EXPECT_EQ(ext["arMethodId"], rig.bank.lookup_id("/api/arar/LArArMethod", "Step-heating - laser"));
  EXPECT_EQ(ext["mineralId"], rig.bank.lookup_id("/api/core/materials", "Sanidine"));
  for (const char* gone : {"arMethodName", "mineralName", "analyticalUncertaintyTypeName", "apparentAgeUncertaintyTypeName",
                           "jvalueUncertaintyUnitName"}) {
    EXPECT_FALSE(ext.contains(gone)) << gone;
  }
  EXPECT_TRUE(ext.contains("analyticalUncertaintyTypeId"));
  // Sent by its name, as the service takes it.
  EXPECT_EQ(ext["lithologyName"], "Rhyolite");

  const json& measurement = store.measurements.at(0).body;
  EXPECT_FALSE(measurement.contains("apparentAgeUncertainityTypeName"));
  EXPECT_TRUE(measurement.contains("apparentAgeUncertainityTypeId"));
  EXPECT_EQ(measurement["plateauStepName"], "No");

  const json& calc = store.age_calcs.at(0).body;
  EXPECT_EQ(calc["decayConstantId"], rig.bank.lookup_id("/api/arar/DecayConstant", "Min et al. 2000"));
  EXPECT_FALSE(calc.contains("decayConstantName"));
  EXPECT_FALSE(calc.contains("fluxMonitorName"));
  EXPECT_TRUE(calc.contains("fluxMonitorId"));
  EXPECT_FALSE(store.age_summaries.at(0).body.contains("ageCalcTypeName"));
}

TEST(Uploader, LookupsAreAskedOnce) {
  Rig rig;
  ASSERT_EQ(rig.run_one(good_submission("S-1", 12)).status, SampleStatus::Complete);
  std::multiset<std::string> asked;
  for (const LoggedRequest& r : rig.bank.requests()) {
    if (r.method != "GET") continue;
    std::string key = r.path;
    for (const auto& [name, value] : r.query) key += "&" + name + "=" + value;
    asked.insert(key);
  }
  for (const std::string& key : asked) EXPECT_EQ(asked.count(key), 1u) << key;
}

TEST(Uploader, SamplesWithAwkwardNamesGetJournalsInsideTheDirectory) {
  Rig rig;
  const std::string name = "A/B & <C> 'x' \"q\" %+ Caf\xC3\xA9 ../..";
  ASSERT_EQ(rig.run_one(good_submission(name)).status, SampleStatus::Complete);
  std::size_t found = 0;
  for (const auto& entry : fs::recursive_directory_iterator(rig.dir.path())) {
    if (!entry.is_regular_file() || entry.path().extension() == ".lock") continue;
    ++found;
    EXPECT_EQ(entry.path().extension(), ".json");
    const fs::path relative = entry.path().lexically_relative(rig.dir.path());
    EXPECT_EQ(std::distance(relative.begin(), relative.end()), 3) << relative;
  }
  EXPECT_EQ(found, 1u);
  EXPECT_TRUE(rig.journal(name).has_value());
  // And the sample keeps the name it has.
  EXPECT_EQ(rig.bank.store().samples.at(0).body["sampleDTO"]["name"], name);
}

TEST(Uploader, ASubmissionTheJournalCannotKeepApartIsRefused) {
  Rig rig;
  Submission twice = good_submission("S-1");
  twice.aliquots.push_back(twice.aliquots[0]);
  const SampleReport report = rig.run_one(twice);
  EXPECT_EQ(report.status, SampleStatus::Refused);
  ASSERT_FALSE(report.messages.empty());
  EXPECT_TRUE(has(report.messages.front(), "20001-01")) << report.messages.front();

  Submission nameless = good_submission("S-1");
  nameless.sample.name.reset();
  EXPECT_EQ(rig.run_one(nameless).status, SampleStatus::Refused);

  const auto both = rig.run({good_submission("S-2"), good_submission("S-2")});
  ASSERT_TRUE(both.has_value());
  EXPECT_EQ(both->samples[0].status, SampleStatus::Complete);
  EXPECT_EQ(both->samples[1].status, SampleStatus::Refused) << "a second submission of one sample name";
  EXPECT_EQ(rig.bank.store().samples.size(), 1u);
}

TEST(Uploader, RefusesToStartWithoutWhatItNeeds) {
  Rig rig;
  rig.options.data_package_id = 0;
  auto report = rig.run({good_submission("S-1")});
  ASSERT_FALSE(report.has_value());
  EXPECT_EQ(report.error().what, Session::no_package_sentence());

  rig.options.data_package_id = rig.bank.writable_package();
  rig.options.journal_dir.clear();
  report = rig.run({good_submission("S-1")});
  ASSERT_FALSE(report.has_value());
  EXPECT_EQ(report.error().kind, ErrorKind::Config);

  rig.options.journal_dir = rig.dir.path();
  for (const char* bad : {"written:", "written:0", "written:-3", "written:x", "yes", "", "not-written "}) {
    rig.options.resolve_in_doubt = bad;
    report = rig.run({good_submission("S-1")});
    EXPECT_FALSE(report.has_value()) << "'" << bad << "'";
  }
  rig.options.resolve_in_doubt.reset();

  FakeEarthBank bank;
  Session out(bank, bank.base_url());
  LookupResolver resolver(out);
  report = upload({good_submission("S-1")}, out, resolver, rig.options);
  ASSERT_FALSE(report.has_value());
  EXPECT_EQ(report.error().code, std::string(kNotSignedIn));
  EXPECT_EQ(bank.count("POST", "/api/"), 0u);
  EXPECT_EQ(rig.posts(), 0u);
}

// ---- Check first -----------------------------------------------------------

TEST(Uploader, StopsBeforeAnyWriteOnMisses) {
  Rig rig;
  Submission bad = good_submission("S-2");
  bad.sample.materialName = "Unobtainium";
  const auto report = rig.run({good_submission("S-1"), bad});
  ASSERT_TRUE(report.has_value());
  ASSERT_FALSE(report->misses.empty());
  EXPECT_EQ(report->misses.front().value, "Unobtainium");
  ASSERT_EQ(report->samples.size(), 2u);
  for (const SampleReport& s : report->samples) {
    EXPECT_EQ(s.status, SampleStatus::Refused) << s.sample;
    EXPECT_FALSE(s.messages.empty()) << s.sample;
  }
  EXPECT_EQ(rig.posts(), 0u) << "not even the good sample";
  EXPECT_EQ(rig.bank.count("GET", "/api/core/sample-with-locations"), 0u);
  EXPECT_TRUE(fs::is_empty(rig.dir.path())) << "and no journal";
}

TEST(Uploader, AllowUnresolvedDropsThoseFields) {
  Rig rig;
  Submission bad = good_submission("S-1");
  bad.sample.materialName = "Unobtainium";
  bad.measurements[1].plateauStepName = "Unobtainium";  // sent by name: not looked up, so not a miss
  rig.options.allow_unresolved = true;
  const SampleReport report = rig.run_one(bad);
  EXPECT_EQ(report.status, SampleStatus::Complete) << ::testing::PrintToString(report.messages);
  EXPECT_TRUE(any_has(report.messages, "Unobtainium")) << ::testing::PrintToString(report.messages);
  const Store store = rig.bank.store();
  const json& sample = store.samples.at(0).body["sampleDTO"];
  EXPECT_FALSE(sample.contains("materialName"));
  EXPECT_FALSE(sample.contains("materialId"));
  EXPECT_EQ(sample["name"], "S-1");
}

TEST(Uploader, AnUnaskableTermStopsEvenWithAllowUnresolved) {
  Rig rig;
  rig.bank.fail_next("GET", "/api/core/materials", 503, "");
  rig.options.allow_unresolved = true;
  const auto report = rig.run({good_submission("S-1")});
  ASSERT_TRUE(report.has_value());
  ASSERT_FALSE(report->misses.empty());
  EXPECT_EQ(report->misses.front().outcome, LookupOutcome::CouldNotAsk);
  EXPECT_EQ(report->samples.at(0).status, SampleStatus::Refused);
  EXPECT_EQ(rig.posts(), 0u);
}

TEST(Uploader, AnUnwritablePackageStopsWithTheSentence) {
  Rig rig;
  rig.options.data_package_id = rig.bank.read_only_package();
  const SampleReport report = rig.run_one(good_submission("S-1"));
  EXPECT_EQ(report.status, SampleStatus::Stopped);
  ASSERT_FALSE(report.messages.empty());
  EXPECT_TRUE(has(report.messages.front(), "package")) << report.messages.front();
  EXPECT_EQ(rig.posts(), 1u) << "one try, and nothing after it";
  EXPECT_EQ(rig.bank.store().samples.size(), 0u);
}

// ---- Failure, and finishing --------------------------------------------------

class FailureAtEachCall : public ::testing::TestWithParam<std::tuple<std::size_t, bool>> {};

TEST_P(FailureAtEachCall, ThenRerunPostsOnlyTheRemainder) {
  const auto [index, never_sent] = GetParam();
  const Call& call = kCalls[index];
  const Submission s = good_submission("S-1");
  Rig rig;
  if (never_sent) {
    rig.bank.refuse_next("POST", call.path, call.skip);
  } else {
    rig.bank.fail_next("POST", call.path, 400, std::string(kRefusal), call.skip);
  }

  const SampleReport first = rig.run_one(s);
  EXPECT_EQ(first.status, index < 2 ? SampleStatus::Stopped : SampleStatus::Partial) << call.label;
  EXPECT_EQ(first.missing.size(), index < 2 ? kPosts - index : 1u) << call.label;
  EXPECT_TRUE(any_has(first.missing, call.label)) << call.label;
  EXPECT_FALSE(first.messages.empty());
  EXPECT_EQ(rig.bank.pending().size(), 0u);
  EXPECT_FALSE(rig.journal("S-1")->in_doubt) << "an answered refusal, or a request that never left, is not in doubt";

  const std::size_t sent = rig.posts();
  const SampleReport second = rig.run_one(s);
  EXPECT_EQ(second.status, SampleStatus::Complete) << call.label;
  EXPECT_EQ(rig.posts() - sent, index < 2 ? kPosts - index : 1u) << call.label;
  // The same records as a clean upload's, none twice (a measurement made
  // after its successors has another id).
  Rig clean;
  ASSERT_EQ(clean.run_one(s).status, SampleStatus::Complete);
  EXPECT_EQ(as_set(rig.bank.store()), as_set(clean.bank.store())) << call.label;
  EXPECT_EQ(rig.bank.store().measurements.size(), 3u) << call.label;

  // Once more: nothing left to send.
  const std::size_t done = rig.posts();
  EXPECT_EQ(rig.run_one(s).status, SampleStatus::Complete);
  EXPECT_EQ(rig.posts(), done);
}

INSTANTIATE_TEST_SUITE_P(EveryPost, FailureAtEachCall,
                         ::testing::Combine(::testing::Range<std::size_t>(0, kPosts), ::testing::Bool()));

TEST(Uploader, TwoSamplesOneFailsTheOtherCompletes) {
  Rig rig;
  rig.bank.fail_next("POST", "/api/arar/ArArDataPoint", 400, std::string(kRefusal));
  const auto report = rig.run({good_submission("S-1"), good_submission("S-2")});
  ASSERT_TRUE(report.has_value());
  ASSERT_EQ(report->samples.size(), 2u);
  EXPECT_EQ(report->samples[0].sample, "S-1");
  EXPECT_EQ(report->samples[0].status, SampleStatus::Stopped);
  EXPECT_EQ(report->samples[1].sample, "S-2");
  EXPECT_EQ(report->samples[1].status, SampleStatus::Complete);
  const Store store = rig.bank.store();
  EXPECT_EQ(store.samples.size(), 2u);
  EXPECT_EQ(store.data_points.size(), 1u);
  EXPECT_EQ(store.measurements.size(), 3u);
}

TEST(Uploader, WhenTheSessionIsLostTheRestOfTheSampleIsNotTried) {
  Rig rig;
  // The token is void and the password is changed: the 401 stands.
  rig.bank.set_password("another-not-a-password");
  rig.bank.expire_tokens();
  const SampleReport report = rig.run_one(good_submission("S-1"));
  EXPECT_NE(report.status, SampleStatus::Complete);
  EXPECT_LE(rig.posts(), 1u);
  EXPECT_EQ(rig.bank.store().samples.size(), 0u);
}

// ---- A write that got no answer --------------------------------------------

TEST(Uploader, ATimeoutAfterSendIsInDoubtAndNothingMoreIsSent) {
  Rig rig;
  rig.bank.drop_next("POST", "/api/arar/ArArAliquot");
  const SampleReport report = rig.run_one(good_submission("S-1"));
  EXPECT_EQ(report.status, SampleStatus::InDoubt);
  ASSERT_FALSE(report.messages.empty());
  const std::string& said = report.messages.front();
  EXPECT_TRUE(has(said, "aliquot 20001-01")) << said;
  EXPECT_TRUE(has(said, "resolve_in_doubt")) << said;
  EXPECT_TRUE(has(said, "written:") && has(said, "not-written")) << said;
  EXPECT_TRUE(has(said, "S-1")) << said;
  // Nothing after it was sent.
  EXPECT_EQ(rig.bank.count("POST", "/api/arar/ArArMeasurement"), 0u);
  EXPECT_EQ(rig.bank.count("POST", "/api/arar/ArArAgeCalc"), 0u);
  EXPECT_EQ(rig.bank.count("POST", "/api/arar/ArArAgeSummary"), 0u);
  // The journal says what, and the data point is in it.
  const auto journal = rig.journal("S-1");
  ASSERT_TRUE(journal.has_value());
  EXPECT_EQ(journal->in_doubt, "aliquot 20001-01");
  EXPECT_TRUE(journal->data_point_id.has_value());
  EXPECT_TRUE(journal->aliquots.empty());
  EXPECT_TRUE(any_has(report.missing, "aliquot 20001-01"));
  EXPECT_TRUE(any_has(report.missing, "age summary"));
}

TEST(Uploader, TheJournalSaysInDoubtBeforeTheRequestIsSent) {
  Rig rig;
  std::optional<std::string> seen;
  bool looked = false;
  rig.hook.before = [&](const HttpRequest& request) {
    if (request.method != "POST" || !has(request.url, "/api/arar/ArArMeasurement") || looked) return;
    looked = true;
    const auto loaded = load_journal(rig.journal_path("S-1"));
    if (loaded && *loaded) seen = (*loaded)->in_doubt;
  };
  ASSERT_EQ(rig.run_one(good_submission("S-1")).status, SampleStatus::Complete);
  EXPECT_TRUE(looked);
  EXPECT_EQ(seen, "measurement 1");
  EXPECT_FALSE(rig.journal("S-1")->in_doubt) << "cleared by the answer";
}

TEST(Uploader, AKilledRunLeavesTheRecordInDoubt) {
  Rig rig;
  std::string snapshot;
  rig.hook.before = [&](const HttpRequest& request) {
    if (request.method == "POST" && has(request.url, "/api/arar/ArArAliquot")) {
      std::ifstream in(rig.journal_path("S-1"), std::ios::binary);
      snapshot.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
  };
  ASSERT_EQ(rig.run_one(good_submission("S-1")).status, SampleStatus::Complete);
  ASSERT_FALSE(snapshot.empty());
  // The process died after the service took the aliquot and before the
  // journal heard of it: the file is what it was when the request went out.
  {
    std::ofstream out(rig.journal_path("S-1"), std::ios::binary | std::ios::trunc);
    out << snapshot;
  }
  const std::size_t sent = rig.posts();
  const SampleReport report = rig.run_one(good_submission("S-1"));
  EXPECT_EQ(report.status, SampleStatus::InDoubt);
  EXPECT_EQ(rig.posts(), sent);
  EXPECT_TRUE(any_has(report.messages, "aliquot 20001-01"));
}

TEST(Uploader, RerunWithoutAResolutionStops) {
  Rig rig;
  rig.bank.drop_next("POST", "/api/arar/ArArDataPoint");
  const SampleReport first = rig.run_one(good_submission("S-1"));
  ASSERT_EQ(first.status, SampleStatus::InDoubt);
  EXPECT_TRUE(has(first.messages.front(), "data point")) << first.messages.front();
  const std::size_t sent = rig.posts();
  const std::size_t requests = rig.bank.requests().size();

  const SampleReport again = rig.run_one(good_submission("S-1"));
  EXPECT_EQ(again.status, SampleStatus::InDoubt);
  ASSERT_FALSE(again.messages.empty());
  EXPECT_EQ(again.messages.front(), first.messages.front()) << "the same words";
  EXPECT_EQ(rig.posts(), sent);
  for (std::size_t i = requests; i < rig.bank.requests().size(); ++i) {
    const LoggedRequest r = rig.bank.requests()[i];
    EXPECT_FALSE(r.path.starts_with("/api/core/sample-with-locations") || r.path.starts_with("/api/arar/ArAr"))
        << "nothing of the sample is asked or sent: " << r;
  }
  EXPECT_EQ(rig.bank.store().data_points.size(), 1u);
}

TEST(Uploader, AGatewayFaultOnAWriteIsInDoubt) {
  for (const int status : {500, 502, 503, 504}) {
    Rig rig;
    rig.bank.fail_next("POST", "/api/arar/ArArMeasurement", status, "", 1);
    const SampleReport report = rig.run_one(good_submission("S-1"));
    EXPECT_EQ(report.status, SampleStatus::InDoubt) << status;
    EXPECT_EQ(rig.journal("S-1")->in_doubt, "measurement 2") << status;
    EXPECT_EQ(rig.bank.store().measurements.size(), 1u);
  }
}

TEST(Uploader, AnAnswerWithoutAnIdIsInDoubt) {
  Rig rig;
  rig.bank.fail_next("POST", "/api/arar/ArArAgeCalc", 201, "{}");
  const SampleReport report = rig.run_one(good_submission("S-1"));
  EXPECT_EQ(report.status, SampleStatus::InDoubt);
  EXPECT_EQ(rig.journal("S-1")->in_doubt, "age calculation");
  EXPECT_EQ(rig.bank.count("POST", "/api/arar/ArArAgeSummary"), 0u);
}

TEST(Uploader, ResolvedAsWrittenContinuesWithThatId) {
  const Submission s = good_submission("S-1");
  Rig rig;
  rig.bank.drop_next("POST", "/api/arar/ArArAliquot");
  ASSERT_EQ(rig.run_one(s).status, SampleStatus::InDoubt);
  const std::int64_t id = rig.bank.store().aliquots.at(0).id;

  rig.options.resolve_in_doubt = "written:" + std::to_string(id);
  const SampleReport report = rig.run_one(s);
  EXPECT_EQ(report.status, SampleStatus::Complete) << ::testing::PrintToString(report.messages);
  const Store store = rig.bank.store();
  EXPECT_EQ(store.aliquots.size(), 1u) << "not sent again";
  EXPECT_EQ(rig.journal("S-1")->aliquots.at("20001-01"), id);
  EXPECT_FALSE(rig.journal("S-1")->in_doubt);
  EXPECT_EQ(dump(store), clean_dump({s}));
  EXPECT_EQ(report.written.size(), kPosts - 3) << "the data point, sample and aliquot were not this call's";
}

TEST(Uploader, ResolvedAsWrittenForTheDataPointHangsTheRestFromIt) {
  const Submission s = good_submission("S-1");
  Rig rig;
  rig.bank.drop_next("POST", "/api/arar/ArArDataPoint");
  ASSERT_EQ(rig.run_one(s).status, SampleStatus::InDoubt);
  const std::int64_t id = rig.bank.store().data_points.at(0).id;
  rig.options.resolve_in_doubt = "written:" + std::to_string(id);
  const SampleReport report = rig.run_one(s);
  EXPECT_EQ(report.status, SampleStatus::Complete);
  EXPECT_EQ(report.data_point_id, id);
  EXPECT_EQ(rig.bank.store().data_points.size(), 1u);
  EXPECT_EQ(dump(rig.bank.store()), clean_dump({s}));
}

TEST(Uploader, ResolvedAsNotWrittenPostsAgain) {
  const Submission s = good_submission("S-1");
  Rig rig;
  rig.bank.lose_next("POST", "/api/arar/ArArMeasurement", ErrorKind::Timeout, 1);
  ASSERT_EQ(rig.run_one(s).status, SampleStatus::InDoubt);
  EXPECT_EQ(rig.bank.store().measurements.size(), 1u) << "it really was not written";

  rig.options.resolve_in_doubt = "not-written";
  const SampleReport report = rig.run_one(s);
  EXPECT_EQ(report.status, SampleStatus::Complete);
  EXPECT_EQ(dump(rig.bank.store()), clean_dump({s}));
  EXPECT_EQ(rig.journal("S-1")->measurements.size(), 3u);
}

TEST(Uploader, AResolutionIsNotKeptForTheNextRun) {
  const Submission s = good_submission("S-1");
  Rig rig;
  rig.bank.drop_next("POST", "/api/arar/ArArAliquot");
  ASSERT_EQ(rig.run_one(s).status, SampleStatus::InDoubt);
  rig.options.resolve_in_doubt = "written:" + std::to_string(rig.bank.store().aliquots.at(0).id);
  ASSERT_EQ(rig.run_one(s).status, SampleStatus::Complete);
  // The same options on a sample that is not in doubt change nothing.
  EXPECT_EQ(rig.run_one(s).status, SampleStatus::Complete);
  EXPECT_EQ(rig.bank.store().aliquots.size(), 1u);
}

TEST(Uploader, ASampleInDoubtIsResolvedBySearch) {
  const Submission s = good_submission("S-1");
  for (const bool written : {true, false}) {
    Rig rig;
    if (written) {
      rig.bank.drop_next("POST", "/api/core/sample-with-locations");
    } else {
      rig.bank.lose_next("POST", "/api/core/sample-with-locations");
    }
    const SampleReport first = rig.run_one(s);
    ASSERT_EQ(first.status, SampleStatus::InDoubt) << written;
    EXPECT_TRUE(has(first.messages.front(), "sample")) << first.messages.front();
    EXPECT_EQ(rig.bank.count("POST", "/api/arar/"), 0u);
    EXPECT_EQ(rig.journal("S-1")->in_doubt, "sample");

    // No resolution is asked of the user: the search settles it.
    const SampleReport second = rig.run_one(s);
    EXPECT_EQ(second.status, SampleStatus::Complete) << written << ::testing::PrintToString(second.messages);
    EXPECT_EQ(rig.bank.store().samples.size(), 1u) << written << ": never two samples";
    EXPECT_EQ(dump(rig.bank.store()), clean_dump({s})) << written;
  }
}

TEST(Uploader, AResolutionNeedsOneSampleInDoubt) {
  const Submission a = good_submission("S-1"), b = good_submission("S-2");
  Rig rig;
  rig.bank.drop_next("POST", "/api/arar/ArArAliquot");
  rig.bank.drop_next("POST", "/api/arar/ArArAliquot", ErrorKind::Timeout, 1);
  const auto first = rig.run({a, b});
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->samples[0].status, SampleStatus::InDoubt);
  EXPECT_EQ(first->samples[1].status, SampleStatus::InDoubt);

  rig.options.resolve_in_doubt = "not-written";
  const std::size_t sent = rig.posts();
  const auto both = rig.run({a, b});
  ASSERT_TRUE(both.has_value());
  EXPECT_EQ(both->samples[0].status, SampleStatus::InDoubt);
  EXPECT_EQ(both->samples[1].status, SampleStatus::InDoubt);
  EXPECT_TRUE(has(both->samples[0].messages.front(), "one at a time")) << both->samples[0].messages.front();
  EXPECT_EQ(rig.posts(), sent) << "a resolution that names no sample is applied to none";

  // Alone, it is applied.
  const auto alone = rig.run({a});
  ASSERT_TRUE(alone.has_value());
  EXPECT_EQ(alone->samples[0].status, SampleStatus::Complete);
}

// ---- Changed data ---------------------------------------------------------

TEST(Uploader, AChangedSubmissionIsRefused) {
  Rig rig;
  ASSERT_EQ(rig.run_one(good_submission("S-1")).status, SampleStatus::Complete);
  Submission changed = good_submission("S-1");
  changed.age_summary.age = 29.0;
  const std::size_t sent = rig.posts();
  const SampleReport report = rig.run_one(changed);
  EXPECT_EQ(report.status, SampleStatus::Refused);
  ASSERT_FALSE(report.messages.empty());
  EXPECT_TRUE(has(report.messages.front(), "changed")) << report.messages.front();
  EXPECT_TRUE(has(report.messages.front(), "new_datapoint")) << report.messages.front();
  EXPECT_EQ(rig.posts(), sent);
}

TEST(Uploader, NewDatapointStartsAfresh) {
  Rig rig;
  ASSERT_EQ(rig.run_one(good_submission("S-1")).status, SampleStatus::Complete);
  const std::string old_text = [&] {
    std::ifstream in(rig.journal_path("S-1"), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }();
  Submission changed = good_submission("S-1");
  changed.age_summary.age = 29.0;
  rig.options.new_datapoint = true;
  const SampleReport report = rig.run_one(changed);
  EXPECT_EQ(report.status, SampleStatus::Complete) << ::testing::PrintToString(report.messages);

  const Store store = rig.bank.store();
  EXPECT_EQ(store.samples.size(), 1u) << "the sample is found again";
  EXPECT_EQ(store.data_points.size(), 2u);
  EXPECT_EQ(store.age_summaries.size(), 2u);
  EXPECT_NE(store.age_summaries[0].body.dump(), store.age_summaries[1].body.dump());

  // The old journal is kept, under a name that says what it is.
  std::vector<fs::path> aside;
  for (const auto& entry : fs::directory_iterator(rig.journal_path("S-1").parent_path())) {
    const std::string name = entry.path().filename().string();
    if (name.ends_with(".superseded.json")) aside.push_back(entry.path());
  }
  ASSERT_EQ(aside.size(), 1u);
  EXPECT_TRUE(aside[0].filename().string().starts_with(rig.journal_path("S-1").stem().string() + ".")) << aside[0];
  std::ifstream in(aside[0], std::ios::binary);
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()), old_text);
  EXPECT_TRUE(any_has(report.messages, "superseded")) << ::testing::PrintToString(report.messages);

  // The new journal is for the new data, and a second restart does not clash with the first aside.
  EXPECT_EQ(rig.journal("S-1")->data_point_id, store.data_points[1].id);
  changed.age_summary.age = 30.0;
  EXPECT_EQ(rig.run_one(changed).status, SampleStatus::Complete);
  std::size_t asides = 0;
  for (const auto& entry : fs::directory_iterator(rig.journal_path("S-1").parent_path())) {
    asides += entry.path().filename().string().ends_with(".superseded.json");
  }
  EXPECT_EQ(asides, 2u);
}

TEST(Uploader, NewDatapointOverAJournalInDoubtSaysSo) {
  Rig rig;
  rig.bank.drop_next("POST", "/api/arar/ArArDataPoint");
  ASSERT_EQ(rig.run_one(good_submission("S-1")).status, SampleStatus::InDoubt);
  Submission changed = good_submission("S-1");
  changed.age_summary.age = 29.0;
  rig.options.new_datapoint = true;
  const SampleReport report = rig.run_one(changed);
  EXPECT_EQ(report.status, SampleStatus::Complete);
  EXPECT_TRUE(any_has(report.messages, "data point")) << ::testing::PrintToString(report.messages);
  EXPECT_TRUE(any_has(report.messages, "may")) << ::testing::PrintToString(report.messages);
}

TEST(Uploader, ChangedDataIsNotRefusedWhenNothingWasWritten) {
  Rig rig;
  rig.bank.fail_next("POST", "/api/core/sample-with-locations", 400, std::string(kRefusal));
  ASSERT_EQ(rig.run_one(good_submission("S-1")).status, SampleStatus::Stopped);
  Submission fixed = good_submission("S-1");
  fixed.age_summary.age = 29.0;
  EXPECT_EQ(rig.run_one(fixed).status, SampleStatus::Complete) << "the journal holds nothing to protect";
}

TEST(Uploader, AJournalThatCannotBeReadRefusesTheSample) {
  Rig rig;
  ASSERT_EQ(rig.run_one(good_submission("S-1")).status, SampleStatus::Complete);
  {
    std::ofstream out(rig.journal_path("S-1"), std::ios::binary | std::ios::trunc);
    out << "{\"version\":1,\"ha";
  }
  const std::size_t sent = rig.posts();
  const SampleReport report = rig.run_one(good_submission("S-1"));
  EXPECT_EQ(report.status, SampleStatus::Refused);
  EXPECT_EQ(rig.posts(), sent) << "a journal that cannot be read is not an empty one";
  ASSERT_FALSE(report.messages.empty());
  EXPECT_TRUE(has(report.messages.front(), rig.journal_path("S-1").filename().string())) << report.messages.front();
  rig.options.new_datapoint = true;
  EXPECT_EQ(rig.run_one(good_submission("S-1")).status, SampleStatus::Refused) << "new_datapoint does not paper over it";
}

// ---- Two uploads at once, and names that differ by case ---------------------

TEST(Uploader, ASampleAnotherUploadHoldsIsRefusedAndNothingIsSentForIt) {
  Rig rig;
  auto held = lock_journal(rig.journal_path("S-1"));
  ASSERT_TRUE(held.has_value());
  const auto report = rig.run({good_submission("S-1"), good_submission("S-2")});
  ASSERT_TRUE(report.has_value());
  EXPECT_EQ(report->samples[0].status, SampleStatus::Refused);
  ASSERT_FALSE(report->samples[0].messages.empty());
  EXPECT_TRUE(has(report->samples[0].messages.front(), "S-1")) << report->samples[0].messages.front();
  EXPECT_TRUE(has(report->samples[0].messages.front(), "another upload")) << report->samples[0].messages.front();
  EXPECT_EQ(report->samples[1].status, SampleStatus::Complete) << "the other samples go on";
  const Store store = rig.bank.store();
  ASSERT_EQ(store.samples.size(), 1u);
  EXPECT_EQ(store.samples[0].body["sampleDTO"]["name"], "S-2");
  EXPECT_EQ(rig.bank.count("GET", "/api/core/sample-with-locations"), 1u) << "only S-2 was searched for";

  // A second uploader in the same directory while the first is mid-run: asked
  // from inside the first's progress callback.
  held->release();
  Rig second_service;
  second_service.options.journal_dir = rig.options.journal_dir;
  std::optional<SampleStatus> inner;
  std::size_t inner_requests = 0;
  rig.options.on_event = [&](const UploadEvent& e) {
    if (e.record != "sample" || inner) return;
    const std::size_t before = second_service.bank.requests().size();
    inner = second_service.run_one(good_submission("S-1")).status;
    inner_requests = second_service.bank.requests().size() - before;
  };
  EXPECT_EQ(rig.run_one(good_submission("S-1")).status, SampleStatus::Complete);
  EXPECT_EQ(inner, SampleStatus::Refused);
  EXPECT_EQ(second_service.bank.count("POST", "/api/core/"), 0u);
  EXPECT_EQ(second_service.posts(), 0u);
  EXPECT_EQ(second_service.bank.count("GET", "/api/core/sample-with-locations"), 0u);
  (void)inner_requests;
  EXPECT_EQ(rig.bank.store().samples.size(), 2u) << "S-2 earlier, S-1 now: never two of one";
}

TEST(Uploader, TheLockIsReleasedWhenTheRunEnds) {
  const auto free = [](Rig& rig, std::string_view sample) {
    return lock_journal(rig.journal_path(sample)).has_value();
  };
  {
    Rig rig;
    ASSERT_EQ(rig.run_one(good_submission("S-1")).status, SampleStatus::Complete);
    EXPECT_TRUE(free(rig, "S-1")) << "after a normal run";
  }
  {
    Rig rig;
    rig.bank.fail_next("POST", "/api/core/sample-with-locations", 400, std::string(kRefusal));
    ASSERT_EQ(rig.run_one(good_submission("S-1")).status, SampleStatus::Stopped);
    EXPECT_TRUE(free(rig, "S-1")) << "after a failure";
  }
  {
    Rig rig;
    rig.bank.drop_next("POST", "/api/arar/ArArAliquot");
    ASSERT_EQ(rig.run_one(good_submission("S-1")).status, SampleStatus::InDoubt);
    EXPECT_TRUE(free(rig, "S-1")) << "after a doubt";
  }
  {
    Rig rig;
    rig.options.cancelled = [] { return true; };
    ASSERT_EQ(rig.run_one(good_submission("S-1")).status, SampleStatus::Stopped);
    EXPECT_TRUE(free(rig, "S-1")) << "after a cancel";
  }
  {
    Rig rig;
    ASSERT_EQ(rig.run_one(good_submission("S-1")).status, SampleStatus::Complete);
    Submission changed = good_submission("S-1");
    changed.age_summary.age = 31.0;
    ASSERT_EQ(rig.run_one(changed).status, SampleStatus::Refused);
    EXPECT_TRUE(free(rig, "S-1")) << "after a refusal";
  }
  {
    Rig rig;
    const auto bad = rig.run({good_submission("S-1")});
    ASSERT_TRUE(bad.has_value());
    rig.options.data_package_id = 0;
    EXPECT_FALSE(rig.run({good_submission("S-1")}).has_value());
    EXPECT_TRUE(free(rig, "S-1")) << "after an error that sent nothing";
  }
}

TEST(Uploader, ALockFileLeftByADeadProcessBlocksNothing) {
  Rig rig;
  fs::path stale = rig.journal_path("S-1");
  stale += ".lock";
  fs::create_directories(stale.parent_path());
  std::ofstream(stale) << "pid 99999";
  EXPECT_EQ(rig.run_one(good_submission("S-1")).status, SampleStatus::Complete);
}

TEST(Uploader, SamplesDifferingOnlyByCaseHaveTheirOwnJournals) {
  Rig rig;
  const auto report = rig.run({good_submission("abc"), good_submission("ABC")});
  ASSERT_TRUE(report.has_value());
  EXPECT_EQ(report->samples[0].status, SampleStatus::Complete);
  EXPECT_EQ(report->samples[1].status, SampleStatus::Complete);
  EXPECT_EQ(rig.bank.store().samples.size(), 2u);
  std::set<std::string> folded;
  for (const auto& entry : fs::directory_iterator(rig.journal_path("abc").parent_path())) {
    std::string name = entry.path().filename().string();
    if (!name.ends_with(".json")) continue;
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    folded.insert(name);
  }
  EXPECT_EQ(folded.size(), 2u);
  EXPECT_NE(rig.journal_path("abc"), rig.journal_path("ABC"));
}

TEST(Uploader, AResolutionNotNeededForTheSampleIsSaidSo) {
  Rig rig;
  rig.bank.drop_next("POST", "/api/core/sample-with-locations");
  ASSERT_EQ(rig.run_one(good_submission("S-1")).status, SampleStatus::InDoubt);
  rig.options.resolve_in_doubt = "not-written";
  const SampleReport report = rig.run_one(good_submission("S-1"));
  EXPECT_EQ(report.status, SampleStatus::Complete);
  EXPECT_TRUE(any_has(report.messages, "resolve_in_doubt was not used")) << ::testing::PrintToString(report.messages);
  EXPECT_TRUE(any_has(report.messages, "searching")) << ::testing::PrintToString(report.messages);
  EXPECT_EQ(rig.bank.store().samples.size(), 1u) << "and the search, not the value, settled it";
}

// ---- Cancel and progress -----------------------------------------------------

TEST(Uploader, CancelStopsBetweenRecordsAndARerunFinishes) {
  const Submission s = good_submission("S-1");
  Rig rig;
  int asked = 0;
  rig.options.cancelled = [&] { return ++asked > 4; };
  const SampleReport first = rig.run_one(s);
  EXPECT_EQ(first.status, SampleStatus::Stopped);
  EXPECT_EQ(rig.posts(), 4u) << "asked before each record";
  EXPECT_EQ(first.written.size(), 4u);
  EXPECT_EQ(first.missing.size(), kPosts - 4);
  EXPECT_TRUE(any_has(first.messages, "your request")) << ::testing::PrintToString(first.messages);
  EXPECT_FALSE(rig.journal("S-1")->in_doubt);

  rig.options.cancelled = nullptr;
  EXPECT_EQ(rig.run_one(s).status, SampleStatus::Complete);
  EXPECT_EQ(dump(rig.bank.store()), clean_dump({s}));
}

TEST(Uploader, CancelBeforeTheFirstRecordSendsNothing) {
  Rig rig;
  rig.options.cancelled = [] { return true; };
  const auto report = rig.run({good_submission("S-1"), good_submission("S-2")});
  ASSERT_TRUE(report.has_value());
  for (const SampleReport& s : report->samples) EXPECT_EQ(s.status, SampleStatus::Stopped) << s.sample;
  EXPECT_EQ(rig.posts(), 0u);
}

TEST(Uploader, ReportsProgress) {
  Rig rig;
  std::vector<UploadEvent> events;
  rig.options.on_event = [&](const UploadEvent& e) { events.push_back(e); };
  ASSERT_EQ(rig.run_one(good_submission("S-1")).status, SampleStatus::Complete);
  ASSERT_GE(events.size(), kPosts + 1);
  EXPECT_EQ(events.front().done, 0u);
  std::size_t last = 0;
  for (const UploadEvent& e : events) {
    EXPECT_EQ(e.sample, "S-1");
    EXPECT_EQ(e.total, kPosts);
    EXPECT_GE(e.done, last);
    last = e.done;
  }
  EXPECT_EQ(events.back().done, events.back().total);
  EXPECT_EQ(events.back().total, rig.posts());
  EXPECT_EQ(events[1].record, "sample");
  EXPECT_EQ(events.back().record, "age summary");

  // A sample that is found is not a POST: the total shrinks with it.
  events.clear();
  rig.options.journal_dir = rig.dir.path() / "other";
  const std::size_t sent = rig.posts();
  ASSERT_EQ(rig.run_one(good_submission("S-1")).status, SampleStatus::Complete);
  EXPECT_EQ(events.back().total, rig.posts() - sent);
  EXPECT_EQ(events.back().done, events.back().total);
}

TEST(Uploader, TokenExpiryMidRunIsInvisible) {
  const Submission s = good_submission("S-1");
  Rig rig;
  int seen = 0;
  rig.options.on_event = [&](const UploadEvent&) {
    if (++seen == 3) rig.bank.expire_tokens();
  };
  const SampleReport report = rig.run_one(s);
  EXPECT_EQ(report.status, SampleStatus::Complete) << ::testing::PrintToString(report.messages);
  EXPECT_EQ(rig.logins(), 2u) << "one sign-in more, made by the session";
  EXPECT_EQ(dump(rig.bank.store()), clean_dump({s})) << "and nothing twice";
}

}  // namespace
}  // namespace pychron::earthbank
