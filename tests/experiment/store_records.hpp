#pragma once

// A valid, finalized AnalysisRecord for the suites that convert records for
// the store. Two signal series (Ar40 on H1, Ar36 on CDD with sigma) and one
// baseline (H1), with the fits the builder asks for.

#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "pychron/experiment/record/builder.hpp"

namespace pychron::experiment::testing {

inline constexpr const char* kSampleUuid = "0190a5e0-7c3b-4d5e-8f10-2a4b6c8d0e12";

// The builder behind sample_record(), for a test that changes something first.
inline record::RecordBuilder sample_builder(std::string identifier = "66573", int aliquot = 1,
                                            std::string mass_spectrometer = "jan") {
  using namespace record;
  RecordBuilder b;
  b.set_identity({kSampleUuid, std::move(identifier), aliquot, "", "unknown", "2026-10-10T12:30:45Z", 3, "q-1"});
  b.set_sample({"FC-2", "proj", "sanidine", "NM-300", "A", "5", "Doe", "a note"});
  b.set_instrument({std::move(mass_spectrometer), "co2", "lab", "jdoe", "0.1.0", "abc123"});
  Extraction ex;
  ex.spec.value = 10.0;
  ex.spec.duration = 30;
  ex.spec.cleanup = 60;
  ex.spec.units = "watts";
  ex.spec.pattern = "circle";
  ex.spec.positions = {4, 7};
  ex.spec.cryo_temperature = 77.0;
  ex.actuals.duration = 30;
  b.set_extraction(ex);
  Measurement m;
  m.plan = {"argon", "3", "[plan]\nx = 1\n", {}};
  m.scripts["extraction"] = {"ext.py", "sha1", "ref1"};
  b.set_measurement(m);
  SpectrometerRec sp;
  sp.state_hash = "hash";
  sp.field_table_version = "ft1";
  sp.integration_time = 1.0;
  sp.gains = {{"H1", 1.0}, {"CDD", 2.0}};
  sp.deflections = {{"CDD", 150.0}};
  sp.source_params = {{"ys", 1.5}};
  b.set_spectrometer(sp);
  b.add_series({"Ar40", "H1", "signal", {{0.f, 1.f, 2.f}, {1.f, 0.9f, 0.8f}, {}}});
  b.add_series({"Ar36", "CDD", "signal", {{0.f, 1.f, 2.f}, {0.1f, 0.2f, 0.3f}, {0.01f, 0.01f, 0.01f}}});
  b.add_series({"Ar40", "H1", "baseline", {{0.f, 1.f}, {0.05f, 0.04f}, {}}});
  InterceptResult ir;
  ir.intercept.value = 1.1;
  ir.intercept.error = 0.01;
  ir.intercept.n_used = 3;
  ir.fit.kind = reduction::FitKind::Linear;
  b.set_intercept("Ar40", ir);
  ir.intercept.value = 0.09;
  b.set_intercept("Ar36", ir);
  BaselineResult br;
  br.value = 0.045;
  br.error = 0.005;
  b.set_baseline("H1", br);
  b.set_icfactor("CDD", 1.02);
  return b;
}

inline record::AnalysisRecord sample_record(std::string identifier = "66573", int aliquot = 1,
                                            std::string mass_spectrometer = "jan") {
  auto r = sample_builder(std::move(identifier), aliquot, std::move(mass_spectrometer)).finalize();
  EXPECT_TRUE(r.has_value());
  return r.value();
}

}  // namespace pychron::experiment::testing
