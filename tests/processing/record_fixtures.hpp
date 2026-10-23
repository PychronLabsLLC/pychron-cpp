#pragma once

// Records directories for the RecordDirectorySource suites (tests/processing
// test_record_source.cpp; tests/arar test_record_loader.cpp): a scratch
// folder per test and an argon record written as the experiment writes it.

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "pychron/experiment/persist/persister.hpp"
#include "pychron/experiment/record/types.hpp"

namespace pychron::processing::test {

namespace rec = pychron::experiment::record;
namespace fs = std::filesystem;

struct TempDir {
  fs::path path;
  TempDir()
      : path(fs::temp_directory_path() /
             ("pp_records_" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()))) {
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~TempDir() { fs::remove_all(path); }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir(TempDir&&) = delete;
  TempDir& operator=(TempDir&&) = delete;
};

inline rec::AnalysisRecord record(const std::string& uuid, const std::string& identifier, int aliquot,
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

inline void write(const fs::path& root, const rec::AnalysisRecord& r) {
  pychron::experiment::persist::FilePersister p(root);
  ASSERT_TRUE(p.save_analysis(r));
  ASSERT_TRUE(p.save_extraction(r));  // extraction files are ignored by the source
}

}  // namespace pychron::processing::test
