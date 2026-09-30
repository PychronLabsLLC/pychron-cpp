#include <gtest/gtest.h>

#include "pychron/devices/spectrometer/detectors.hpp"

namespace {

using namespace pychron;
using namespace pychron::spectrometer;

DetectorConfig faraday(std::string name, std::string channel) {
  DetectorConfig c;
  c.name = std::move(name);
  c.channel = std::move(channel);
  c.units = "fA";
  return c;
}

std::vector<DetectorConfig> sample_configs() {
  auto h1 = faraday("H1", "qtegra:H1");
  h1.isotope = "Ar40";
  h1.deflection = DeflectionConfig{true, {0.0, 0.0012}, 1, 800.0, 0.0031};
  h1.protection = ProtectionConfig{5e5, true};
  h1.saturation = 4.9e6;
  auto ax = faraday("AX", "qtegra:AX");
  ax.active = false;
  DetectorConfig cdd;
  cdd.name = "CDD";
  cdd.kind = DetectorKind::Cdd;
  cdd.channel = "counter:0";
  cdd.units = "cps";
  cdd.dead_time_ns = 25;
  cdd.cdd_voltage = 1450;
  return {h1, ax, cdd};
}

TEST(DetectorKind, NamesRoundTripAndUnknownIsConfig) {
  for (auto k : {DetectorKind::Faraday, DetectorKind::Counter, DetectorKind::Cdd,
                 DetectorKind::Atona}) {
    auto parsed = parse_detector_kind(to_string(k));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, k);
  }
  auto bad = parse_detector_kind("bolometer");
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().kind, ErrorKind::Config);
}

TEST(DetectorKind, ApplicableCaps) {
  EXPECT_TRUE(applicable_caps(DetectorKind::Faraday).has(DetectorCap::Gain));
  EXPECT_FALSE(applicable_caps(DetectorKind::Faraday).has(DetectorCap::CddVoltage));
  EXPECT_TRUE(applicable_caps(DetectorKind::Cdd).has(DetectorCap::CddVoltage));
  EXPECT_FALSE(applicable_caps(DetectorKind::Counter).has(DetectorCap::Gain));
  for (auto k : {DetectorKind::Faraday, DetectorKind::Counter, DetectorKind::Cdd,
                 DetectorKind::Atona}) {
    EXPECT_TRUE(applicable_caps(k).has(DetectorCap::Protect)) << to_string(k);
  }
}

TEST(DeflectionConfig, ClampsToMax) {
  DeflectionConfig d;
  EXPECT_DOUBLE_EQ(d.clamp(5000.0), 5000.0);
  d.max = 800.0;
  EXPECT_DOUBLE_EQ(d.clamp(5000.0), 800.0);
  EXPECT_DOUBLE_EQ(d.clamp(-5000.0), -800.0);
  EXPECT_DOUBLE_EQ(d.clamp(12.0), 12.0);
}

TEST(DetectorSet, InitialStateComesFromConfig) {
  auto set = DetectorSet::create(sample_configs());
  ASSERT_TRUE(set.has_value());
  EXPECT_EQ(set->size(), 3U);

  auto h1 = set->state("H1");
  ASSERT_TRUE(h1.has_value());
  EXPECT_TRUE(h1->active);
  EXPECT_EQ(h1->isotope, "Ar40");
  EXPECT_FALSE(h1->protected_);
  EXPECT_FALSE(h1->deflection.has_value());

  EXPECT_FALSE(set->state("AX")->active);
  EXPECT_EQ(set->state("CDD")->cdd_voltage, 1450.0);
  EXPECT_EQ(set->active(), (std::vector<DetectorId>{"H1", "CDD"}));
}

TEST(DetectorSet, LooksUpByNameAndChannel) {
  auto set = DetectorSet::create(sample_configs());
  ASSERT_TRUE(set.has_value());
  ASSERT_NE(set->find("CDD"), nullptr);
  EXPECT_EQ(set->find("CDD")->kind, DetectorKind::Cdd);
  ASSERT_NE(set->by_channel("qtegra:AX"), nullptr);
  EXPECT_EQ(set->by_channel("qtegra:AX")->name, "AX");
  EXPECT_EQ(set->find("L2"), nullptr);
  EXPECT_EQ(set->by_channel("qtegra:L2"), nullptr);
}

TEST(DetectorSet, RejectsDuplicateAndEmptyNamesAndChannelsCollectingAll) {
  std::vector<DetectorConfig> configs{faraday("H1", "a"), faraday("H1", "b"), faraday("L1", "a"),
                                      faraday("", "c"), faraday("L2", "")};
  auto set = DetectorSet::create(configs);
  ASSERT_FALSE(set.has_value());
  EXPECT_EQ(set.error().kind, ErrorKind::Config);
  const auto& what = set.error().what;
  EXPECT_NE(what.find("duplicate detector 'H1'"), std::string::npos) << what;
  EXPECT_NE(what.find("channel 'a'"), std::string::npos) << what;
  EXPECT_NE(what.find("empty name"), std::string::npos) << what;
  EXPECT_NE(what.find("'L2' has no channel"), std::string::npos) << what;
}

TEST(DetectorSet, MutationsPublishStateOnlyWhenChanged) {
  ManualClock clock(TimePoint{} + std::chrono::seconds(10));
  auto set = DetectorSet::create(sample_configs(), &clock);
  ASSERT_TRUE(set.has_value());
  std::vector<DetectorState> events;
  set->on_change([&](const DetectorState& s) { events.push_back(s); });

  clock.advance(std::chrono::seconds(1));
  ASSERT_TRUE(set->set_active("AX", true).has_value());
  ASSERT_TRUE(set->set_active("AX", true).has_value());  // no change, no event
  ASSERT_TRUE(set->set_isotope("H1", "Ar39").has_value());
  ASSERT_TRUE(set->record_protected("H1", true).has_value());
  ASSERT_TRUE(set->record_deflection("H1", 25.0).has_value());
  ASSERT_TRUE(set->record_gain("H1", 1.01).has_value());
  ASSERT_TRUE(set->record_cdd_voltage("CDD", 1500.0).has_value());

  ASSERT_EQ(events.size(), 6U);
  EXPECT_EQ(events[0].detector, "AX");
  EXPECT_TRUE(events[0].active);
  EXPECT_EQ(events[0].ts, TimePoint{} + std::chrono::seconds(11));
  EXPECT_EQ(events[1].isotope, "Ar39");
  EXPECT_TRUE(events[2].protected_);
  EXPECT_EQ(events[3].deflection, 25.0);
  EXPECT_EQ(events[4].gain, 1.01);
  EXPECT_EQ(events[5].detector, "CDD");
  EXPECT_EQ(events[5].cdd_voltage, 1500.0);

  auto h1 = set->state("H1");
  ASSERT_TRUE(h1.has_value());
  EXPECT_EQ(*h1, events[4]);
}

TEST(DetectorSet, ConfigIsNotMutatedByRuntimeState) {
  auto set = DetectorSet::create(sample_configs());
  ASSERT_TRUE(set.has_value());
  ASSERT_TRUE(set->set_active("H1", false).has_value());
  ASSERT_TRUE(set->set_isotope("H1", "Ar36").has_value());
  EXPECT_TRUE(set->find("H1")->active);
  EXPECT_EQ(set->find("H1")->isotope, "Ar40");
  EXPECT_FALSE(set->state("H1")->active);
}

TEST(DetectorSet, UnknownDetectorIsConfigError) {
  auto set = DetectorSet::create(sample_configs());
  ASSERT_TRUE(set.has_value());
  auto s = set->state("L9");
  ASSERT_FALSE(s.has_value());
  EXPECT_EQ(s.error().kind, ErrorKind::Config);
  auto r = set->record_protected("L9", true);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
}

}  // namespace
