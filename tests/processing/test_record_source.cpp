#include "pychron/processing/record_source.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "pychron/experiment/persist/persister.hpp"
#include "pychron/experiment/record/serialize.hpp"
#include "pychron/processing/quantity.hpp"
#include "pychron/processing/reduced.hpp"

namespace pp = pychron::processing;
namespace rec = pychron::experiment::record;
namespace fs = std::filesystem;

namespace {

struct TempDir {
  fs::path path;
  TempDir() {
    path = fs::temp_directory_path() /
           ("pp_records_" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~TempDir() { fs::remove_all(path); }
};

rec::AnalysisRecord record(const std::string& uuid, const std::string& identifier, int aliquot,
                           const std::string& step, const std::string& type, const std::string& ts,
                           double ar40 = 3000.0, double ar36 = 10.0) {
  rec::AnalysisRecord r;
  r.identity = {uuid, identifier, aliquot, step, type, ts, 1, "q-1"};
  r.sample = {"FC-2", "proj", "sanidine", "NM-300", "A", "5", "Doe", ""};
  r.instrument = {"jan", "co2", "lab", "me", "0.1.0", "abc"};
  r.extraction.spec = {10.0, 30, 60, "W", "", {1}, 77.0};
  r.extraction.actuals.cryo_measured = {{"A", 77.2}};
  r.extraction.actuals.value = 9.5;
  r.spectrometer.gains = {{"H1", 1.002}};
  r.spectrometer.deflections = {{"H1", 10.0}};
  auto add = [&](const char* iso, const char* det, double v, double e) {
    rec::DataSeries s{iso, det, "signal", {{0.f, 1.f, 2.f}, {static_cast<float>(v), static_cast<float>(v), static_cast<float>(v)}, {}}};
    r.data.series.push_back(s);
    rec::InterceptResult ir;
    ir.intercept.value = v;
    ir.intercept.error = e;
    ir.intercept.n_used = 3;
    r.results.intercepts[iso] = ir;
  };
  add("Ar40", "H1", ar40, 0.5);
  add("Ar39", "AX", 100.0, 0.1);
  add("Ar38", "L1", 2.0, 0.01);
  add("Ar37", "L2", 0.5, 0.01);
  add("Ar36", "CDD", ar36, 0.02);
  r.data.series.push_back({"", "H1", "baseline", {{0.f, 1.f}, {0.01f, 0.01f}, {}}});
  r.results.baselines["H1"] = rec::BaselineResult{0.01, 0.001, {}};
  r.results.icfactors["CDD"] = 1.05;
  return r;
}

void write(const fs::path& root, const rec::AnalysisRecord& r) {
  pychron::experiment::persist::FilePersister p(root);
  ASSERT_TRUE(p.save_analysis(r));
  ASSERT_TRUE(p.save_extraction(r));  // extraction files are ignored by the source
}

TEST(RecordSource, ParsesTimestampsAndSteps) {
  EXPECT_DOUBLE_EQ(*pp::parse_utc("1970-01-01T00:00:00Z"), 0.0);
  EXPECT_DOUBLE_EQ(*pp::parse_utc("2023-11-14T22:13:20Z"), 1'700'000'000.0);
  EXPECT_DOUBLE_EQ(*pp::parse_utc("2023-11-14 22:13:20.25Z"), 1'700'000'000.25);
  EXPECT_FALSE(pp::parse_utc("2023-13-14T22:13:20Z"));
  EXPECT_FALSE(pp::parse_utc("yesterday"));
  EXPECT_EQ(pp::increment_from_step(""), -1);
  EXPECT_EQ(pp::increment_from_step("A"), 0);
  EXPECT_EQ(pp::increment_from_step("Z"), 25);
  EXPECT_EQ(pp::increment_from_step("AA"), 26);
  EXPECT_EQ(pp::step_letters(26), "AA");
  EXPECT_EQ(pp::make_runid("66001", 3, 1), "66001-03B");
}

TEST(RecordSource, MapsARecord) {
  const auto a = pp::analysis_from_record(record("u1", "66001", 2, "B", "unknown", "2026-09-30T12:00:00Z"));
  EXPECT_EQ(a.runid, "66001-02B");
  EXPECT_EQ(a.increment, 1);
  EXPECT_EQ(a.irradiation, "NM-300");
  EXPECT_EQ(*a.extraction.value, 9.5);
  EXPECT_EQ(a.extraction.cryo_temperature, 77.0);
  EXPECT_EQ(a.extraction.cryo_measured, (std::map<std::string, double>{{"A", 77.2}}));
  ASSERT_EQ(a.isotopes.size(), 5u);
  EXPECT_EQ(a.isotopes[0].key, "Ar40");
  EXPECT_EQ(a.isotopes[4].key, "Ar36");
  EXPECT_EQ(a.isotopes[0].detector, "H1");
  EXPECT_DOUBLE_EQ(a.isotopes[0].baseline.value, 0.01);
  EXPECT_DOUBLE_EQ(a.isotopes[4].ic_factor.value, 1.05);
  EXPECT_EQ(a.isotopes[0].n, 3);
  const auto raw = pp::raw_from_record(record("u1", "66001", 2, "B", "unknown", "2026-09-30T12:00:00Z"));
  ASSERT_NE(raw.find(pp::SeriesKind::Signal, "Ar40"), nullptr);
  ASSERT_NE(raw.find(pp::SeriesKind::Baseline, "H1"), nullptr);
  EXPECT_EQ(raw.find(pp::SeriesKind::Signal, "Ar40")->t.size(), 3u);
}

// A failed run's record (spec 4.5) is an invalid analysis, as the store
// gives it; truncated and complete are ok.
TEST(RecordSource, AFailedRecordIsAnInvalidAnalysis) {
  auto r = record("u1", "66001", 2, "B", "unknown", "2026-09-30T12:00:00Z");
  EXPECT_EQ(pp::analysis_from_record(r).tag, "ok");
  r.provenance.status = rec::RecordStatus::Truncated;
  EXPECT_EQ(pp::analysis_from_record(r).tag, "ok");
  r.provenance.status = rec::RecordStatus::Failed;
  r.provenance.failure = "spectrometer link down";
  EXPECT_EQ(pp::analysis_from_record(r).tag, "invalid");
}

// A detector intercalibration: Ar40 hopped onto H1 and AX. The record keeps
// the acquisition's names ("Ar40:H1"); the analysis has the isotope under its
// detector, as every other analysis of the kind is read ("H1:Ar40").
TEST(RecordSource, AnIsotopeOnSeveralDetectorsIsOneIsotopeADetector) {
  auto r = record("u-ic", "ic", 1, "", "detector_ic", "2026-01-01T00:00:00Z");
  r.data.series.clear();
  r.results.intercepts.clear();
  auto add = [&](const char* det, const char* key, float v) {
    r.data.series.push_back({"Ar40", det, "signal", {{0.f, 1.f, 2.f}, {v, v, v}, {}}});
    rec::InterceptResult ir;
    ir.intercept.value = v;
    ir.intercept.error = 0.5;
    ir.intercept.n_used = 3;
    r.results.intercepts[key] = ir;
  };
  add("AX", "Ar40:AX", 2990.f);
  add("H1", "Ar40:H1", 3000.f);
  r.data.series.push_back({"Ar40", "H1", "sniff", {{0.f}, {3000.f}, {}}});
  r.data.series.push_back({"", "H1", "baseline", {{9.f}, {0.01f}, {}}});
  r.data.series.push_back({"", "AX", "baseline", {{9.f}, {0.02f}, {}}});
  r.results.baselines["H1"] = rec::BaselineResult{0.01, 0.001, {}};
  r.results.baselines["AX"] = rec::BaselineResult{0.02, 0.002, {}};
  r.results.icfactors = {{"AX", 1.01}};

  const auto a = pp::analysis_from_record(r);
  ASSERT_EQ(a.isotopes.size(), 2u);
  EXPECT_EQ(a.isotopes[0].key, "AX:Ar40");
  EXPECT_EQ(a.isotopes[0].isotope, "Ar40");
  EXPECT_EQ(a.isotopes[0].detector, "AX");
  EXPECT_DOUBLE_EQ(a.isotopes[0].intercept.value, 2990.0);
  EXPECT_DOUBLE_EQ(a.isotopes[0].baseline.value, 0.02);
  EXPECT_DOUBLE_EQ(a.isotopes[0].ic_factor.value, 1.01);
  EXPECT_EQ(a.isotopes[1].key, "H1:Ar40");
  EXPECT_EQ(a.isotopes[1].isotope, "Ar40");
  EXPECT_EQ(a.isotopes[1].detector, "H1");
  EXPECT_DOUBLE_EQ(a.isotopes[1].baseline.value, 0.01);
  ASSERT_NE(a.find_isotope("H1:Ar40"), nullptr);
  EXPECT_EQ(a.find_isotope("H1:Ar40")->detector, "H1");

  const auto raw = pp::raw_from_record(r);
  ASSERT_NE(raw.find(pp::SeriesKind::Signal, "H1:Ar40"), nullptr);
  EXPECT_EQ(raw.find(pp::SeriesKind::Signal, "H1:Ar40")->detector, "H1");
  ASSERT_NE(raw.find(pp::SeriesKind::Signal, "AX:Ar40"), nullptr);
  EXPECT_EQ(raw.find(pp::SeriesKind::Signal, "AX:Ar40")->v.front(), 2990.0);
  EXPECT_EQ(raw.find(pp::SeriesKind::Signal, "Ar40"), nullptr);
  // The sniff of an isotope is found by the isotope's key.
  ASSERT_NE(raw.find(pp::SeriesKind::Sniff, "H1:Ar40"), nullptr);
}

// A baseline block and a baseline hop on one detector are that detector's one
// raw baseline, in the order the acquisition fitted them.
TEST(RecordSource, ADetectorsBaselineSeriesAreOneRawSeries) {
  auto r = record("u-bh", "66001", 1, "", "unknown", "2026-01-01T00:00:00Z");
  r.data.series.push_back({"Ar36", "H1", "baseline", {{3.f, 4.f}, {0.03f, 0.04f}, {}}});
  const auto raw = pp::raw_from_record(r);
  int baselines = 0;
  for (const auto& s : raw.series) baselines += s.kind == pp::SeriesKind::Baseline ? 1 : 0;
  EXPECT_EQ(baselines, 1);
  const auto* h1 = raw.find(pp::SeriesKind::Baseline, "H1");
  ASSERT_NE(h1, nullptr);
  EXPECT_EQ(h1->t, (std::vector<double>{0.0, 1.0, 3.0, 4.0}));
  ASSERT_EQ(h1->v.size(), 4u);
  EXPECT_FLOAT_EQ(static_cast<float>(h1->v[3]), 0.04f);
}

TEST(RecordSource, BrowsesNewestFirstWithPaging) {
  TempDir dir;
  for (int i = 0; i < 7; ++i)
    write(dir.path, record("u" + std::to_string(i), i < 4 ? "66001" : "air", i + 1, "", i < 4 ? "unknown" : "air",
                           "2026-09-30T1" + std::to_string(i) + ":00:00Z"));
  pp::RecordDirectorySource src(dir.path);
  pp::BrowseQuery q;
  q.limit = 3;
  auto p1 = src.browse(q);
  ASSERT_TRUE(p1) << p1.error().what;
  EXPECT_TRUE(src.problems().empty());
  ASSERT_EQ(p1->rows.size(), 3u);
  EXPECT_EQ(p1->rows[0].uuid, "u6");
  EXPECT_EQ(*p1->total, 7u);
  ASSERT_TRUE(p1->next);
  q.after = p1->next;
  auto p2 = src.browse(q);
  ASSERT_TRUE(p2);
  EXPECT_EQ(p2->rows[0].uuid, "u3");
  q.after = p2->next;
  auto p3 = src.browse(q);
  ASSERT_TRUE(p3);
  ASSERT_EQ(p3->rows.size(), 1u);
  EXPECT_FALSE(p3->next);

  pp::BrowseQuery types;
  types.analysis_types = {"air"};
  EXPECT_EQ(src.browse(types)->rows.size(), 3u);
  pp::BrowseQuery text;
  text.text = "6600";
  EXPECT_EQ(src.browse(text)->rows.size(), 4u);
  pp::BrowseQuery recent;
  recent.last_hours = 2.5;
  EXPECT_EQ(src.browse(recent)->rows.size(), 3u);

  // Facets ignore their own filter.
  auto f = src.facet(pp::Facet::AnalysisType, types);
  ASSERT_TRUE(f);
  EXPECT_EQ(*f, (std::vector<std::string>{"air", "unknown"}));
  pp::BrowseQuery only_air = types;
  auto ids = src.facet(pp::Facet::Identifier, only_air);
  EXPECT_EQ(*ids, (std::vector<std::string>{"air"}));
}

TEST(RecordSource, RefreshPicksUpNewFilesAndBumpsGeneration) {
  TempDir dir;
  write(dir.path, record("u1", "66001", 1, "", "unknown", "2026-09-30T10:00:00Z"));
  pp::RecordDirectorySource src(dir.path);
  ASSERT_TRUE(src.refresh());
  const auto g1 = src.generation();
  ASSERT_TRUE(src.refresh());
  EXPECT_EQ(src.generation(), g1);
  write(dir.path, record("u2", "66001", 2, "", "unknown", "2026-09-30T11:00:00Z"));
  ASSERT_TRUE(src.refresh());
  EXPECT_GT(src.generation(), g1);
  EXPECT_EQ(src.browse({})->rows.size(), 2u);

  std::ofstream(dir.path / "66001" / "broken.json") << "{ nope";
  ASSERT_TRUE(src.refresh());
  EXPECT_EQ(src.problems().size(), 1u);
  EXPECT_EQ(src.browse({})->rows.size(), 2u);
}

// The processing bridge refreshes the source on its worker while a window's
// job may still load on another thread: refresh is safe against both.
TEST(RecordSource, ARefreshWhileOtherThreadsReadIsSafe) {
  TempDir dir;
  write(dir.path, record("u1", "66001", 1, "", "unknown", "2026-09-30T10:00:00Z"));
  write(dir.path, record("u2", "66001", 2, "", "unknown", "2026-09-30T11:00:00Z"));
  pp::RecordDirectorySource src(dir.path);
  ASSERT_TRUE(src.refresh());
  const auto g0 = src.generation();
  std::atomic<bool> go{false};
  std::atomic<int> failed{0};
  std::vector<std::thread> readers;
  readers.reserve(4);
  for (int i = 0; i < 4; ++i)
    readers.emplace_back([&, i] {
      while (!go) std::this_thread::yield();
      for (int k = 0; k < 10; ++k) {
        if (!src.load((i + k) % 2 ? "u1" : "u2")) ++failed;
        if (!src.browse({})) ++failed;
      }
    });
  std::thread refresher([&] {
    while (!go) std::this_thread::yield();
    for (int k = 0; k < 10; ++k)
      if (!src.refresh()) ++failed;
  });
  write(dir.path, record("u3", "66001", 3, "", "unknown", "2026-09-30T12:00:00Z"));
  go = true;
  for (auto& t : readers) t.join();
  refresher.join();
  EXPECT_EQ(failed.load(), 0);
  ASSERT_TRUE(src.refresh());
  EXPECT_GT(src.generation(), g0);
  EXPECT_EQ(src.browse({})->rows.size(), 3u);
}

TEST(RecordSource, LoadsAnalysesAndRawAndAppliesReferences) {
  TempDir dir;
  write(dir.path, record("u1", "66001", 1, "", "unknown", "2026-09-30T10:00:00Z", 1000.0, 0.5));
  std::ofstream(dir.path / "references.toml") << R"(
[flux."66001"]
j = 0.001
j_err = 1e-6

[production."NM-300"]
K4039 = [0.0002, 0.00001]
Ca3937 = [0.0007, 0.00001]

[[chronology."NM-300"]]
power = 1.0
start = "2026-01-01T00:00:00Z"
end = "2026-01-01T10:00:00Z"
)";
  pp::RecordDirectorySource src(dir.path);
  auto a = src.load("u1");
  ASSERT_TRUE(a) << a.error().what;
  ASSERT_TRUE((*a)->context.flux);
  EXPECT_DOUBLE_EQ((*a)->context.flux->j.value, 0.001);
  ASSERT_TRUE((*a)->context.production);
  EXPECT_EQ((*a)->context.chronology.size(), 1u);
  auto ra = pp::reduce_analysis(*a, {});
  auto age = pp::Quantity::parse("age")->eval(*ra);
  ASSERT_TRUE(age) << ra->reduction_error;
  EXPECT_GT(age->value, 0.0);

  auto raw = src.load_raw("u1");
  ASSERT_TRUE(raw);
  EXPECT_EQ(raw->series.size(), 6u);
  EXPECT_FALSE(src.load("missing"));
}

}  // namespace
