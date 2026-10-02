#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>

#include "pychron/systems/spectrometer/data_dir.hpp"
#include "pychron/systems/spectrometer/spectrometer.hpp"
#include "spectrometer_fakes.hpp"

using namespace pychron;
using namespace pychron::spectrometer;
using namespace pychron::spectrometer::testing;
using namespace std::chrono_literals;

namespace {

const std::filesystem::path kIntegrated =
    std::filesystem::path(PYCHRON_EXAMPLE_CONFIGS_DIR) / "spectrometer.sim-integrated.toml";

// sim-integrated config and tables, but every role is a fake.
struct Rig {
  CallLog log;
  ManualClock clock{TimePoint{} + 100s};
  SignalBus bus;
  Scheduler scheduler{clock, &bus, Scheduler::Options{0}};
  FakePositioner positioner{log};
  FakeControl control{log};
  FakeBlank blank{log};
  FakeSource source;
  FakeAcquirer acquirer{{"H2", "H1", "AX", "L1", "L2", "CDD"}};
  std::vector<Duration> sleeps;
  std::vector<MagnetMoved> moves;
  std::vector<DetectorState> states;
  SignalBus::Subscription s1, s2;
  std::unique_ptr<Spectrometer> spec;

  std::optional<cfg::Limits> magnet_limits;  // replaces [magnet].limits when set
  bool af_demag = false;                     // enables [magnet].af_demag with a 0.5 swing

  cfg::SpectrometerData data() {
    auto d = cfg::load_spectrometer(kIntegrated);
    EXPECT_TRUE(d.has_value()) << (d ? "" : d.error().what);
    if (magnet_limits) d->config.magnet.limits = magnet_limits;
    if (af_demag) d->config.magnet.af_demag = cfg::AfDemag{true, 0.4, 0.4, 0.5, 0.5};
    return std::move(*d);
  }

  Result<std::unique_ptr<Spectrometer>> make(std::map<std::string, FieldTable> extra = {}, bool with_control = true) {
    auto d = data();
    std::map<std::string, FieldTable> tables;
    for (const auto& [name, tf] : d.tables) tables.emplace(name, to_field_table(tf));
    for (auto& [name, t] : extra) tables[name] = t;
    SpectrometerRoles roles;
    roles.positioner = &positioner;
    roles.source = &source;
    roles.acquirers = {{"sim", &acquirer}};
    roles.detector_control = with_control ? &control : nullptr;
    roles.beam_blank = &blank;
    Spectrometer::Options options;
    options.sleep = [this](Duration dt) {
      sleeps.push_back(dt);
      clock.advance(dt);
    };
    return Spectrometer::create(d.config, MolecularWeights(d.weights), std::move(tables), std::move(roles),
                                SpectrometerContext{clock, scheduler, bus}, options);
  }

  void build(std::map<std::string, FieldTable> extra = {}) {
    s1 = bus.subscribe<MagnetMoved>([this](const MagnetMoved& m) { moves.push_back(m); });
    s2 = bus.subscribe<DetectorState>([this](const DetectorState& s) { states.push_back(s); });
    auto made = make(std::move(extra));
    ASSERT_TRUE(made.has_value()) << made.error().what;
    spec = std::move(*made);
  }
};

}  // namespace

TEST(Spectrometer, CreateRequiresPositioner) {
  Rig r;
  auto d = r.data();
  std::map<std::string, FieldTable> tables;
  for (const auto& [name, tf] : d.tables) tables.emplace(name, to_field_table(tf));
  SpectrometerRoles roles;
  roles.acquirers = {{"sim", &r.acquirer}};
  auto made = Spectrometer::create(d.config, MolecularWeights(d.weights), tables, std::move(roles),
                                   SpectrometerContext{r.clock, r.scheduler, r.bus});
  ASSERT_FALSE(made.has_value());
  EXPECT_EQ(made.error().kind, ErrorKind::Config);
  EXPECT_NE(made.error().what.find("positioner"), std::string::npos);
}

TEST(Spectrometer, PositionIsotopeRunsPipeline) {
  Rig r;
  r.build();
  r.source.hv = 4500.0 * 1.21;  // HV factor 1.1
  auto result = r.spec->position(PositionTarget{Isotope{"Ar40"}, "H1"});
  ASSERT_TRUE(result.has_value()) << result.error().what;
  EXPECT_NEAR(*result->mass, 39.9623831237, 1e-9);
  EXPECT_NEAR(*result->table_value, 5.001, 1e-9);
  EXPECT_NEAR(result->native, 5.001 * 1.1, 1e-9);
  EXPECT_NEAR(r.positioner.value, 5.001 * 1.1, 1e-9);
}

TEST(Spectrometer, DeflectionCorrectionUsesRuntimeDeflectionAndRoundTrips) {
  Rig r;
  r.build();
  auto clamped = r.spec->set_deflection("L1", 100.0);
  ASSERT_TRUE(clamped.has_value());
  auto native = r.spec->native_for(39.9623831237, "L1");
  ASSERT_TRUE(native.has_value());
  EXPECT_NEAR(*native, 4.750 - 0.0012 * 100.0, 1e-9);  // L1 sign = -1
  auto mass = r.spec->mass_at(*native, "L1");
  ASSERT_TRUE(mass.has_value());
  EXPECT_NEAR(*mass, 39.9623831237, 1e-6);
  EXPECT_NEAR(*r.spec->uncorrect(*r.spec->correct(4.2, "H2"), "H2"), 4.2, 1e-12);
}

TEST(Spectrometer, HvReadbackCachedForOneSecond) {
  Rig r;
  r.build();
  ASSERT_TRUE(r.spec->correct(5.0, "H1").has_value());
  const int reads = r.source.hv_reads;
  ASSERT_TRUE(r.spec->correct(5.0, "H1").has_value());
  EXPECT_EQ(r.source.hv_reads, reads);
  r.clock.advance(1500ms);
  ASSERT_TRUE(r.spec->correct(5.0, "H1").has_value());
  EXPECT_EQ(r.source.hv_reads, reads + 1);
}

TEST(Spectrometer, MagnetMovedCarriesMassOnReference) {
  Rig r;
  r.build();
  ASSERT_TRUE(r.spec->position(PositionTarget{Isotope{"Ar40"}, "H1"}).has_value());
  ASSERT_EQ(r.moves.size(), 1U);
  EXPECT_DOUBLE_EQ(r.moves[0].from, 0.0);
  EXPECT_NEAR(r.moves[0].to, 5.001, 1e-9);
  EXPECT_EQ(r.moves[0].axis, IMassPositioner::Axis::Dac);
  ASSERT_TRUE(r.moves[0].mass_on_reference.has_value());
  EXPECT_NEAR(*r.moves[0].mass_on_reference, 39.9623831237, 1e-6);
  EXPECT_EQ(r.moves[0].elapsed, 500ms);  // settle_ms from config
}

TEST(Spectrometer, LargeMoveProtectsAndBlanksThenReleases) {
  Rig r;
  r.build();
  ASSERT_TRUE(r.spec->move_native(5.0).has_value());  // from 0: above beam_blank_threshold 0.5
  EXPECT_EQ(r.log, (CallLog{"protect:CDD", "blank", "set:5.000", "unblank", "unprotect:CDD"}));
  // DetectorState events: CDD protected, then released.
  ASSERT_EQ(r.states.size(), 2U);
  EXPECT_EQ(r.states[0].detector, "CDD");
  EXPECT_TRUE(r.states[0].protected_);
  EXPECT_FALSE(r.states[1].protected_);
  EXPECT_FALSE(r.spec->detector_state("CDD")->protected_);
}

TEST(Spectrometer, SmallMoveProtectsOnlyWhenPeakOnPath) {
  Rig r;
  r.build();
  r.positioner.value = 5.001;
  ASSERT_TRUE(r.spec->move_native(5.101).has_value());  // no CDD peak in [4.95, 5.15]
  EXPECT_EQ(r.log, (CallLog{"set:5.101"}));
  r.log.clear();
  r.positioner.value = 4.9;
  ASSERT_TRUE(r.spec->move_native(4.52).has_value());  // CDD Ar40 peak at 4.505 within margin
  EXPECT_EQ(r.log, (CallLog{"protect:CDD", "set:4.520", "unprotect:CDD"}));
  r.log.clear();
  PositionOptions never;
  never.protect = ProtectPolicy::Never;
  ASSERT_TRUE(r.spec->move_native(4.9, never).has_value());
  EXPECT_EQ(r.log, (CallLog{"set:4.900"}));
}

// A correction that cannot be computed (here the HV read behind it times out)
// leaves protection unplannable: the move is refused before anything is sent.
TEST(Spectrometer, FailedCorrectionWhilePlanningProtectionAbortsTheMove) {
  Rig r;
  r.build();
  r.positioner.value = 4.9;
  r.source.fail_read_hv = true;
  auto moved = r.spec->move_native(4.52);  // CDD Ar40 peak at 4.505 is on the path
  ASSERT_FALSE(moved.has_value());
  EXPECT_EQ(moved.error().kind, ErrorKind::Timeout);
  EXPECT_TRUE(r.log.empty());  // no protect, no blank, no set
  EXPECT_TRUE(r.positioner.sets.empty());
  EXPECT_FALSE(r.blank.blanked);
  EXPECT_TRUE(r.moves.empty());

  auto positioned = r.spec->position(PositionTarget{NativeUnits{4.52}, ""});
  ASSERT_FALSE(positioned.has_value());
  EXPECT_EQ(positioned.error().kind, ErrorKind::Timeout);
  EXPECT_TRUE(r.log.empty());

  // Once HV reads again the same move is planned and protected as usual.
  r.source.fail_read_hv = false;
  ASSERT_TRUE(r.spec->move_native(4.52).has_value());
  EXPECT_EQ(r.log, (CallLog{"protect:CDD", "set:4.520", "unprotect:CDD"}));
}

// Nothing is planned from the table for these moves, so no correction is
// needed and a failing HV read does not stop them.
TEST(Spectrometer, MovesThatNeedNoCorrectionIgnoreAFailingHvRead) {
  Rig r;
  r.build();
  r.source.fail_read_hv = true;
  PositionOptions never;
  never.protect = ProtectPolicy::Never;
  ASSERT_TRUE(r.spec->move_native(4.52, never).has_value());
  r.log.clear();
  ASSERT_TRUE(r.spec->move_native(9.0).has_value());  // large: protected and blanked without the table
  EXPECT_EQ(r.log, (CallLog{"protect:CDD", "blank", "set:9.000", "unblank", "unprotect:CDD"}));
}

// The AF demag swing is clamped to the facade's effective limits
// ([magnet].limits narrowing the positioner's 0..10), not the positioner's.
TEST(Spectrometer, AfDemagSwingIsClampedToConfigLimits) {
  Rig r;
  r.magnet_limits = cfg::Limits{0.0, 6.0};
  r.af_demag = true;
  r.build();
  r.positioner.value = 3.0;
  ASSERT_TRUE(r.spec->move_native(5.9).has_value());
  ASSERT_GT(r.positioner.sets.size(), 1U);  // demag steps, then the target
  EXPECT_DOUBLE_EQ(*std::max_element(r.positioner.sets.begin(), r.positioner.sets.end()), 6.0);  // it swung past
  for (double v : r.positioner.sets) EXPECT_LE(v, 6.0);
  EXPECT_DOUBLE_EQ(r.positioner.sets.back(), 5.9);
}

TEST(Spectrometer, FailedMoveLeavesNothingProtected) {
  Rig r;
  r.build();
  r.positioner.fail_set_at = 1;
  auto moved = r.spec->move_native(5.0);
  ASSERT_FALSE(moved.has_value());
  EXPECT_FALSE(r.control.any_protected());
  EXPECT_FALSE(r.blank.blanked);
  EXPECT_FALSE(r.spec->detector_state("CDD")->protected_);
  EXPECT_TRUE(r.moves.empty());
}

TEST(Spectrometer, MoveOutsideLimitsIsRejected) {
  Rig r;
  r.build();
  auto moved = r.spec->move_native(12.0);
  ASSERT_FALSE(moved.has_value());
  EXPECT_EQ(moved.error().kind, ErrorKind::Config);
  EXPECT_TRUE(r.log.empty());
}

// [magnet].limits and the positioner's limits (0..10 here) both apply; on
// each side the stricter bound wins.
TEST(Spectrometer, ConfigLimitsNarrowerThanPositionerRefuseTheMove) {
  Rig r;
  r.magnet_limits = cfg::Limits{2.0, 6.0};
  r.build();
  for (double v : {7.0, 1.0}) {
    auto moved = r.spec->move_native(v);
    ASSERT_FALSE(moved.has_value()) << v;
    EXPECT_EQ(moved.error().kind, ErrorKind::Config);
    auto positioned = r.spec->position(PositionTarget{NativeUnits{v}, ""});
    ASSERT_FALSE(positioned.has_value()) << v;
    EXPECT_EQ(positioned.error().kind, ErrorKind::Config);
  }
  EXPECT_TRUE(r.log.empty());
  EXPECT_TRUE(r.spec->move_native(6.0).has_value());
  EXPECT_TRUE(r.spec->move_native(2.0).has_value());
}

TEST(Spectrometer, ConfigLimitsWiderThanPositionerDoNotWidenIt) {
  Rig r;
  r.magnet_limits = cfg::Limits{-5.0, 20.0};
  r.build();
  for (double v : {12.0, -1.0}) {
    auto moved = r.spec->move_native(v);
    ASSERT_FALSE(moved.has_value()) << v;
    EXPECT_EQ(moved.error().kind, ErrorKind::Config);
  }
  EXPECT_TRUE(r.log.empty());
  EXPECT_TRUE(r.spec->move_native(10.0).has_value());
}

TEST(Spectrometer, ConfigLimitsApplyWhenPositionerHasNone) {
  Rig r;
  r.positioner.lim = Limits{1.0, 0.0};  // invalid: the positioner declares no limits
  r.magnet_limits = cfg::Limits{0.0, 6.0};
  r.build();
  auto moved = r.spec->move_native(7.0);
  ASSERT_FALSE(moved.has_value());
  EXPECT_EQ(moved.error().kind, ErrorKind::Config);
  EXPECT_TRUE(r.log.empty());
  EXPECT_TRUE(r.spec->move_native(5.0).has_value());
}

TEST(Spectrometer, DisjointConfigAndPositionerLimitsRefuseEveryMove) {
  Rig r;
  r.magnet_limits = cfg::Limits{11.0, 12.0};
  r.build();
  for (double v : {5.0, 11.5}) {
    auto moved = r.spec->move_native(v);
    ASSERT_FALSE(moved.has_value()) << v;
    EXPECT_EQ(moved.error().kind, ErrorKind::Config);
  }
  EXPECT_TRUE(r.log.empty());
}

TEST(Spectrometer, UnknownIsotopeAndDiscreteMissAreErrorsNotFallbacks) {
  Rig r;
  r.build();
  EXPECT_FALSE(r.spec->position(PositionTarget{Isotope{"Xe999"}, "H1"}).has_value());
  EXPECT_FALSE(r.spec->position(PositionTarget{Isotope{"Ar40"}, "NOPE"}).has_value());
  EXPECT_TRUE(r.positioner.sets.empty());
}

TEST(Spectrometer, WithTableScopesTheActiveTable) {
  Rig r;
  FieldTable ic(FitKind::Discrete, TableAxis::Dac, {ControlPoint{"Ar40", 39.9623831237, {{"H1", 6.0}}}});
  r.build({{"ic", ic}});
  {
    auto scope = r.spec->with_table("ic");
    ASSERT_TRUE(scope.has_value());
    EXPECT_EQ(r.spec->active_table(), "ic");
    auto pos = r.spec->position(PositionTarget{Isotope{"Ar40"}, "H1"});
    ASSERT_TRUE(pos.has_value());
    EXPECT_NEAR(pos->native, 6.0, 1e-9);
  }
  EXPECT_EQ(r.spec->active_table(), "argon");
  auto pos = r.spec->position(PositionTarget{Isotope{"Ar40"}, "H1"});
  ASSERT_TRUE(pos.has_value());
  EXPECT_NEAR(pos->native, 5.001, 1e-9);
  EXPECT_FALSE(r.spec->with_table("missing").has_value());
}

TEST(Spectrometer, UpdateTableShiftsSubsequentPositions) {
  Rig r;
  r.build();
  ASSERT_TRUE(r.spec->update_table("H1", "Ar40", 5.011).has_value());
  auto pos = r.spec->position(PositionTarget{Isotope{"Ar40"}, "H1"});
  ASSERT_TRUE(pos.has_value());
  EXPECT_NEAR(pos->native, 5.011, 1e-9);
  // propagate defaults to [magnet].propagate = false
  EXPECT_NEAR(*r.spec->field_table().value_for(39.9623831237, "AX"), 4.874, 1e-9);
}

TEST(Spectrometer, PositionHvUsesHvTableAndSource) {
  Rig r;
  FieldTable hv(FitKind::Linear, TableAxis::Dac,
                {ControlPoint{"Ar40", 39.9623831237, {{"H1", 4500.0}}},
                 ControlPoint{"Ar36", 35.967545105, {{"H1", 5000.0}}}});
  r.build({{"argon_hv", hv}});
  auto pos = r.spec->position_hv(35.967545105, "H1");
  ASSERT_TRUE(pos.has_value()) << pos.error().what;
  EXPECT_NEAR(r.source.hv, 5000.0, 1e-6);
  EXPECT_NEAR(pos->native, 5000.0, 1e-6);
  EXPECT_TRUE(r.positioner.sets.empty());
}

TEST(Spectrometer, DetectorOpsGuardedByCaps) {
  Rig r;
  r.control.caps_ = DetectorCap::Protect;
  r.build();
  auto defl = r.spec->set_deflection("H1", 10.0);
  ASSERT_FALSE(defl.has_value());
  EXPECT_EQ(defl.error().kind, ErrorKind::Config);
  EXPECT_FALSE(r.spec->set_gain("H1", 2.0).has_value());
  EXPECT_FALSE(r.spec->set_cdd_voltage("CDD", 1400.0).has_value());
  EXPECT_TRUE(r.spec->protect("H1", true).has_value());
  EXPECT_TRUE(r.spec->detector_state("H1")->protected_);
}

TEST(Spectrometer, DeflectionClampedToMax) {
  Rig r;
  r.build();
  auto defl = r.spec->set_deflection("H1", 2000.0);
  ASSERT_TRUE(defl.has_value());
  EXPECT_DOUBLE_EQ(*defl, 800.0);
  EXPECT_DOUBLE_EQ(r.control.deflection["H1"], 800.0);
  EXPECT_EQ(*r.spec->detector_state("H1")->deflection, 800.0);
}

TEST(Spectrometer, NoDetectorControlMeansUnsupported) {
  Rig r;
  auto made = r.make({}, false);
  ASSERT_TRUE(made.has_value()) << made.error().what;
  auto p = (*made)->protect("H1", true);
  ASSERT_FALSE(p.has_value());
  EXPECT_EQ(p.error().kind, ErrorKind::Config);
  EXPECT_FALSE((*made)->has_detector_control());
}

TEST(Spectrometer, SnapshotIsContentHashed) {
  Rig r;
  r.build();
  ASSERT_TRUE(r.spec->move_native(5.0).has_value());
  auto a = r.spec->snapshot();
  r.clock.advance(10s);
  auto b = r.spec->snapshot();
  EXPECT_EQ(a.hash, b.hash);  // ts differs, content does not
  EXPECT_EQ(a.hash, content_hash(a));
  EXPECT_EQ(a.hash_hex().size(), 16U);
  EXPECT_EQ(*a.magnet, 5.0);
  EXPECT_EQ(*a.hv, 4500.0);
  EXPECT_EQ(a.params.size(), 2U);
  EXPECT_EQ(a.detectors.size(), 6U);
  EXPECT_EQ(a.field_table, "argon");

  ASSERT_TRUE(r.spec->set_deflection("H1", 12.0).has_value());
  auto c = r.spec->snapshot();
  EXPECT_NE(c.hash, a.hash);
  ASSERT_TRUE(r.spec->set_deflection("H1", 0.0).has_value());
  EXPECT_EQ(r.spec->snapshot().hash, a.hash);
}
