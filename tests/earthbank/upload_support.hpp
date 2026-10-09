// What the tests of the journal and the uploader share: a temporary
// directory and a submission whose every term the fake service knows.
#pragma once

#include <atomic>
#include <filesystem>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

#include "pychron/earthbank/model.hpp"
#include "pychron/earthbank/vocab.hpp"

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace pychron::earthbank::testing {

// A directory of its own, removed with it. Named for the process and a count,
// so that two build trees running at once do not meet.
class TempDir {
 public:
  TempDir() {
    static std::atomic<unsigned> serial{0};
#ifdef _WIN32
    const long pid = static_cast<long>(_getpid());
#else
    const long pid = static_cast<long>(getpid());
#endif
    path_ = std::filesystem::temp_directory_path() /
            ("pychron_upload_" + std::to_string(pid) + "_" + std::to_string(serial.fetch_add(1)));
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

// A submission as the mapping makes one: every term a name the fixtures'
// lists hold, no id set. One aliquot and `measurements` steps of it.
inline Submission good_submission(const std::string& sample, int measurements = 3) {
  Submission s;
  s.datapoint_name = sample + " Sanidine";
  s.setup_name = "Noble gas lab";
  s.irradiation_name = "IRR-1";
  s.sample.name = sample;
  s.sample.sampleID = sample;
  s.sample.materialName = "Sanidine";
  s.location = Location{};
  s.location->name = sample;
  s.location->lat = -34.9;
  s.location->lon = 138.6;
  s.data_point.analysisDate = "2026-05-20";
  s.data_point.analysisUnits = std::string(kIntensityUnits);
  s.data_point.arMethodName = "Step-heating - laser";
  s.data_point.mineralName = "Sanidine";
  s.data_point.lithologyName = "Rhyolite";
  s.data_point.analyticalUncertaintyTypeName = std::string(kOneSigma);
  s.data_point.analyticalUncertaintyUnitName = std::string(kAbsoluteUncertainty);
  s.data_point.apparentAgeUncertaintyTypeName = std::string(kOneSigma);
  s.data_point.apparentAgeUncertaintyUnitName = std::string(kAbsoluteUncertainty);
  s.data_point.jvalueUncertaintyUnitName = std::string(kAbsoluteUncertainty);
  Aliquot aliquot;
  aliquot.aliquotName = "20001-01";
  s.aliquots.push_back(aliquot);
  for (int i = 0; i < measurements; ++i) {
    Measurement m;
    m.aliquotName = "20001-01";
    m.stepNumber = i + 1;
    m.apparentAge = 28.2 + i;
    m.apparentAgeUncertainityTypeName = std::string(kOneSigma);
    m.jvalueUncertaintyTypeName = std::string(kOneSigma);
    m.plateauStepName = std::string(i == 0 ? kNo : kYes);
    s.measurements.push_back(m);
  }
  s.age_calc.decayConstantName = "Min et al. 2000";
  s.age_calc.airRatioName = "Lee et al. 2006";
  s.age_calc.fluxMonitorName = flux_monitor_query("FC-2", 28.201);
  s.age_summary.age = 28.2;
  s.age_summary.ageCalcTypeName = std::string(age_type_name(AgeKind::Plateau));
  s.age_summary.ageUncertaintyTypeName = std::string(kOneSigma);
  s.age_summary.calculatedAgeUncertaintyTypeName = std::string(kOneSigma);
  s.age_summary.calculatedAgeUncertaintyTypeUnitsName = std::string(kAbsoluteUncertainty);
  s.age_summary.dataInterpretationToolName = std::string(kInterpretationTool);
  s.age_summary.interpretationName = std::string(kUnknownInterpretation);
  return s;
}

}  // namespace pychron::earthbank::testing
