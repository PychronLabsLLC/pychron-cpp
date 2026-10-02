#pragma once

// Spectrometer role conformance suites. A new role driver proves its contract
// by instantiating the suites for the roles it implements with a harness type
// that builds the driver on scripted/simulated transport:
//
//   struct MyDacHarness {
//     MyDacHarness();                               // builds driver + SimTransport
//     IMassPositioner& positioner();
//     // optional: double tolerance() const;         // read-back tolerance
//     // optional: void advance();                   // let simulated time pass
//   };
//   INSTANTIATE_TYPED_TEST_SUITE_P(MyDac, PositionerConformance,
//                                  ::testing::Types<MyDacHarness>);
//
// Harness members per suite:
//   PositionerConformance       IMassPositioner& positioner()
//   AcquirerConformance         IIntensityAcquirer& acquirer()
//   SourceConformance           IBeamSource& source()
//   DetectorControlConformance  IDetectorControl& detector_control(), ChannelId channel()
// Optional on any harness: tolerance(), advance() (called between polls, e.g.
// to step a ManualClock or feed a SimTransport), timeout() (next() timeout).

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>
#include <string>

#include "pychron/devices/spectrometer/roles.hpp"

namespace pychron::spectrometer::conformance {

template <class H>
double tolerance(const H& h) {
  if constexpr (requires { h.tolerance(); }) {
    return h.tolerance();
  } else {
    return 1e-9;
  }
}

template <class H>
void advance(H& h) {
  if constexpr (requires { h.advance(); }) h.advance();
}

template <class H>
Duration timeout(const H& h) {
  if constexpr (requires { h.timeout(); }) {
    return h.timeout();
  } else {
    return std::chrono::seconds(2);
  }
}

inline bool near(double a, double b, double tol) {
  return std::fabs(a - b) <= tol * std::max({1.0, std::fabs(a), std::fabs(b)});
}

constexpr int kPollLimit = 1000;

}  // namespace pychron::spectrometer::conformance

// --- IMassPositioner --------------------------------------------------------

template <class H>
class PositionerConformance : public ::testing::Test {
 protected:
  H harness;
};
TYPED_TEST_SUITE_P(PositionerConformance);

TYPED_TEST_P(PositionerConformance, LimitsAreAValidNonEmptyRange) {
  auto limits = this->harness.positioner().limits();
  EXPECT_LT(limits.min, limits.max);
}

TYPED_TEST_P(PositionerConformance, SetWithinLimitsReadsBack) {
  using namespace pychron::spectrometer::conformance;
  auto& p = this->harness.positioner();
  auto limits = p.limits();
  for (double f : {0.25, 0.5, 0.75}) {
    const double target = limits.min + f * (limits.max - limits.min);
    ASSERT_TRUE(p.set(target).has_value()) << "target " << target;
    for (int i = 0; i < kPollLimit; ++i) {
      auto moving = p.moving();
      ASSERT_TRUE(moving.has_value());
      if (!*moving) break;
      advance(this->harness);
    }
    auto value = p.read();
    ASSERT_TRUE(value.has_value());
    EXPECT_TRUE(near(*value, target, tolerance(this->harness)))
        << "read " << *value << " after set " << target;
  }
}

TYPED_TEST_P(PositionerConformance, SetOutsideLimitsFailsWithConfigAndDoesNotMove) {
  auto& p = this->harness.positioner();
  auto limits = p.limits();
  const double inside = limits.min + 0.5 * (limits.max - limits.min);
  ASSERT_TRUE(p.set(inside).has_value());
  auto before = p.read();
  ASSERT_TRUE(before.has_value());

  const double span = limits.max - limits.min;
  for (double outside : {limits.max + span, limits.min - span}) {
    auto r = p.set(outside);
    ASSERT_FALSE(r.has_value()) << "set " << outside << " should fail";
    EXPECT_EQ(r.error().kind, pychron::ErrorKind::Config);
    auto after = p.read();
    ASSERT_TRUE(after.has_value());
    EXPECT_DOUBLE_EQ(*after, *before);
  }
}

TYPED_TEST_P(PositionerConformance, MovingSettles) {
  using namespace pychron::spectrometer::conformance;
  auto& p = this->harness.positioner();
  auto limits = p.limits();
  ASSERT_TRUE(p.set(limits.min + 0.1 * (limits.max - limits.min)).has_value());
  ASSERT_TRUE(p.set(limits.min + 0.9 * (limits.max - limits.min)).has_value());
  bool settled = false;
  for (int i = 0; i < kPollLimit && !settled; ++i) {
    auto moving = p.moving();
    ASSERT_TRUE(moving.has_value());
    settled = !*moving;
    if (!settled) advance(this->harness);
  }
  EXPECT_TRUE(settled);
}

REGISTER_TYPED_TEST_SUITE_P(PositionerConformance, LimitsAreAValidNonEmptyRange,
                            SetWithinLimitsReadsBack,
                            SetOutsideLimitsFailsWithConfigAndDoesNotMove, MovingSettles);

// --- IIntensityAcquirer -----------------------------------------------------

template <class H>
class AcquirerConformance : public ::testing::Test {
 protected:
  H harness;

  // Up to `n` frames after start(); fails the test on error or timeout.
  std::vector<pychron::spectrometer::Frame> collect(int n) {
    using namespace pychron::spectrometer::conformance;
    auto& a = harness.acquirer();
    std::vector<pychron::spectrometer::Frame> frames;
    for (int i = 0; i < kPollLimit && static_cast<int>(frames.size()) < n; ++i) {
      EXPECT_TRUE(a.trigger().has_value());
      advance(harness);
      auto f = a.next(timeout(harness));
      EXPECT_TRUE(f.has_value()) << (f ? "" : pychron::to_string(f.error()));
      if (!f) break;
      if (*f) frames.push_back(std::move(**f));
    }
    EXPECT_EQ(static_cast<int>(frames.size()), n);
    return frames;
  }
};
TYPED_TEST_SUITE_P(AcquirerConformance);

TYPED_TEST_P(AcquirerConformance, ChannelsAreNonEmptyAndUnique) {
  auto channels = this->harness.acquirer().channels();
  ASSERT_FALSE(channels.empty());
  std::set<std::string> unique(channels.begin(), channels.end());
  EXPECT_EQ(unique.size(), channels.size());
  for (const auto& c : channels) EXPECT_FALSE(c.empty());
}

TYPED_TEST_P(AcquirerConformance, NonPositiveIntegrationIsRejected) {
  auto& a = this->harness.acquirer();
  for (auto d : {pychron::Duration::zero(), -std::chrono::duration_cast<pychron::Duration>(
                                                   std::chrono::seconds(1))}) {
    auto r = a.configure(d);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, pychron::ErrorKind::Config);
  }
}

TYPED_TEST_P(AcquirerConformance, FramesHaveMonotonicSeqAndTs) {
  auto& a = this->harness.acquirer();
  ASSERT_TRUE(a.configure(std::chrono::seconds(1)).has_value());
  ASSERT_TRUE(a.start().has_value());
  auto frames = this->collect(5);
  ASSERT_TRUE(a.stop().has_value());
  for (std::size_t i = 1; i < frames.size(); ++i) {
    EXPECT_GT(frames[i].seq, frames[i - 1].seq) << "frame " << i;
    EXPECT_GE(frames[i].ts, frames[i - 1].ts) << "frame " << i;
  }
}

TYPED_TEST_P(AcquirerConformance, FramesMatchDeclaredShape) {
  auto& a = this->harness.acquirer();
  auto channels = a.channels();
  std::set<std::string> known(channels.begin(), channels.end());
  ASSERT_TRUE(a.configure(std::chrono::seconds(1)).has_value());
  ASSERT_TRUE(a.start().has_value());
  auto frames = this->collect(3);
  ASSERT_TRUE(a.stop().has_value());
  for (const auto& f : frames) {
    EXPECT_EQ(f.integrated, a.integrates());
    EXPECT_GT(f.span, pychron::Duration::zero());
    EXPECT_FALSE(f.values.empty());
    std::set<std::string> seen;
    for (const auto& [channel, value] : f.values) {
      EXPECT_TRUE(known.contains(channel)) << "undeclared channel " << channel;
      EXPECT_TRUE(seen.insert(channel).second) << "duplicate channel " << channel;
      EXPECT_TRUE(std::isfinite(value)) << channel;
    }
  }
}

TYPED_TEST_P(AcquirerConformance, RestartsAfterStop) {
  auto& a = this->harness.acquirer();
  ASSERT_TRUE(a.configure(std::chrono::seconds(1)).has_value());
  ASSERT_TRUE(a.start().has_value());
  auto first = this->collect(1);
  ASSERT_TRUE(a.stop().has_value());
  ASSERT_TRUE(a.start().has_value());
  auto second = this->collect(1);
  ASSERT_TRUE(a.stop().has_value());
  ASSERT_EQ(first.size(), 1U);
  ASSERT_EQ(second.size(), 1U);
  EXPECT_GT(second[0].seq, first[0].seq);
}

REGISTER_TYPED_TEST_SUITE_P(AcquirerConformance, ChannelsAreNonEmptyAndUnique,
                            NonPositiveIntegrationIsRejected, FramesHaveMonotonicSeqAndTs,
                            FramesMatchDeclaredShape, RestartsAfterStop);

// --- IBeamSource ------------------------------------------------------------

template <class H>
class SourceConformance : public ::testing::Test {
 protected:
  H harness;
};
TYPED_TEST_SUITE_P(SourceConformance);

TYPED_TEST_P(SourceConformance, ParamsAreUniqueNamedAndValid) {
  auto specs = this->harness.source().params();
  std::set<std::string> ids;
  std::set<std::string> vendor;
  for (const auto& s : specs) {
    const auto id = pychron::spectrometer::to_string(s.id);
    EXPECT_TRUE(ids.insert(id).second) << "duplicate param " << id;
    EXPECT_FALSE(s.vendor_name.empty()) << id;
    EXPECT_TRUE(vendor.insert(s.vendor_name).second) << "duplicate vendor name " << s.vendor_name;
    EXPECT_TRUE(s.range.valid()) << id;
    EXPECT_TRUE(s.readable || s.writable) << id;
  }
}

TYPED_TEST_P(SourceConformance, HvSetReadsBack) {
  using namespace pychron::spectrometer;
  auto& src = this->harness.source();
  double target = 4500.0;
  if (const auto* hv = find_spec(src.params(), ParamId{SourceParam::HV})) {
    target = hv->range.min + 0.5 * (hv->range.max - hv->range.min);
  }
  ASSERT_TRUE(src.set_hv(target).has_value());
  conformance::advance(this->harness);
  auto hv = src.read_hv();
  ASSERT_TRUE(hv.has_value());
  EXPECT_TRUE(conformance::near(*hv, target, conformance::tolerance(this->harness)))
      << "read " << *hv << " after set " << target;
}

TYPED_TEST_P(SourceConformance, WritableParamsReadBackSetpoint) {
  using namespace pychron::spectrometer;
  auto& src = this->harness.source();
  for (const auto& s : src.params()) {
    if (!s.writable) continue;
    const double target = s.range.min + 0.5 * (s.range.max - s.range.min);
    ASSERT_TRUE(src.set_param(s.id, target).has_value()) << to_string(s.id);
    if (!s.readable) continue;
    conformance::advance(this->harness);
    auto rb = src.read_param(s.id);
    ASSERT_TRUE(rb.has_value()) << to_string(s.id);
    EXPECT_TRUE(conformance::near(rb->setpoint, target, conformance::tolerance(this->harness)))
        << to_string(s.id) << " setpoint " << rb->setpoint << " after set " << target;
    if (rb->actual) {
      EXPECT_TRUE(std::isfinite(*rb->actual)) << to_string(s.id);
    }
  }
}

TYPED_TEST_P(SourceConformance, ReadOnlyParamsRejectWritesAndWriteOnlyRejectReads) {
  using namespace pychron::spectrometer;
  auto& src = this->harness.source();
  for (const auto& s : src.params()) {
    if (!s.writable) {
      auto r = src.set_param(s.id, s.range.min);
      ASSERT_FALSE(r.has_value()) << to_string(s.id);
      EXPECT_EQ(r.error().kind, pychron::ErrorKind::Config);
    }
    if (!s.readable) {
      auto r = src.read_param(s.id);
      ASSERT_FALSE(r.has_value()) << to_string(s.id);
      EXPECT_EQ(r.error().kind, pychron::ErrorKind::Config);
    }
  }
}

TYPED_TEST_P(SourceConformance, UnadvertisedParamsFailWithConfig) {
  using namespace pychron::spectrometer;
  auto& src = this->harness.source();
  std::vector<ParamId> missing{ParamId{Custom{"__conformance_unsupported__"}}};
  for (const auto& info : source_params()) {
    if (!find_spec(src.params(), ParamId{info.param})) missing.emplace_back(info.param);
  }
  for (const auto& id : missing) {
    auto w = src.set_param(id, 1.0);
    ASSERT_FALSE(w.has_value()) << to_string(id);
    EXPECT_EQ(w.error().kind, pychron::ErrorKind::Config) << to_string(id);
    auto r = src.read_param(id);
    ASSERT_FALSE(r.has_value()) << to_string(id);
    EXPECT_EQ(r.error().kind, pychron::ErrorKind::Config) << to_string(id);
  }
}

REGISTER_TYPED_TEST_SUITE_P(SourceConformance, ParamsAreUniqueNamedAndValid, HvSetReadsBack,
                            WritableParamsReadBackSetpoint,
                            ReadOnlyParamsRejectWritesAndWriteOnlyRejectReads,
                            UnadvertisedParamsFailWithConfig);

// --- IDetectorControl -------------------------------------------------------

template <class H>
class DetectorControlConformance : public ::testing::Test {
 protected:
  H harness;
};
TYPED_TEST_SUITE_P(DetectorControlConformance);

TYPED_TEST_P(DetectorControlConformance, OpsOutsideCapsFailWithConfig) {
  using namespace pychron::spectrometer;
  auto& dc = this->harness.detector_control();
  const auto ch = this->harness.channel();
  const Caps caps = dc.caps();
  auto expect_config = [](const auto& r, const char* op) {
    ASSERT_FALSE(r.has_value()) << op;
    EXPECT_EQ(r.error().kind, pychron::ErrorKind::Config) << op;
  };
  if (!caps.has(DetectorCap::Deflection)) {
    expect_config(dc.set_deflection(ch, 1.0), "set_deflection");
    expect_config(dc.read_deflection(ch), "read_deflection");
  }
  if (!caps.has(DetectorCap::Gain)) {
    expect_config(dc.set_gain(ch, 1.0), "set_gain");
    expect_config(dc.read_gain(ch), "read_gain");
  }
  if (!caps.has(DetectorCap::CddVoltage)) expect_config(dc.set_cdd_voltage(ch, 1.0), "set_cdd");
  if (!caps.has(DetectorCap::Protect)) expect_config(dc.protect(ch, true), "protect");
}

TYPED_TEST_P(DetectorControlConformance, SupportedOpsRoundTrip) {
  using namespace pychron::spectrometer;
  auto& dc = this->harness.detector_control();
  const auto ch = this->harness.channel();
  const Caps caps = dc.caps();
  const double tol = conformance::tolerance(this->harness);
  if (caps.has(DetectorCap::Deflection)) {
    ASSERT_TRUE(dc.set_deflection(ch, 12.5).has_value());
    auto d = dc.read_deflection(ch);
    ASSERT_TRUE(d.has_value());
    EXPECT_TRUE(conformance::near(*d, 12.5, tol)) << *d;
  }
  if (caps.has(DetectorCap::Gain)) {
    ASSERT_TRUE(dc.set_gain(ch, 1.05).has_value());
    auto g = dc.read_gain(ch);
    ASSERT_TRUE(g.has_value());
    EXPECT_TRUE(conformance::near(*g, 1.05, tol)) << *g;
  }
  if (caps.has(DetectorCap::Protect)) {
    EXPECT_TRUE(dc.protect(ch, true).has_value());
    EXPECT_TRUE(dc.protect(ch, false).has_value());
  }
  if (caps.has(DetectorCap::CddVoltage)) {
    EXPECT_TRUE(dc.set_cdd_voltage(ch, 1450.0).has_value());
  }
}

REGISTER_TYPED_TEST_SUITE_P(DetectorControlConformance, OpsOutsideCapsFailWithConfig,
                            SupportedOpsRoundTrip);
