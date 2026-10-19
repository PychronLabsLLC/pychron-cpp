#include <gtest/gtest.h>

#include <chrono>

#include "pychron/experiment/collect/collector.hpp"
#include "pychron/reduction/arar.hpp"

using namespace pychron;
using namespace pychron::experiment;
using namespace pychron::experiment::collect;
using namespace std::chrono_literals;

namespace {

spectrometer::Reading reading(TimePoint ts, std::map<std::string, double> values) {
  spectrometer::Reading r;
  r.ts = ts;
  r.integration = 1s;
  for (auto& [det, v] : values) r.values[det] = spectrometer::Value{v, std::nullopt, std::nullopt, false};
  return r;
}

SeriesKey skey(std::string label, std::string detector, std::string_view kind) {
  SeriesKey k;
  k.label = std::move(label);
  k.detector = std::move(detector);
  k.kind = std::string(kind);
  return k;
}

class CollectorTest : public ::testing::Test {
 protected:
  void SetUp() override { collector_.start(t0_); }

  static CollectionSpec spec(std::string_view kind, int counts, std::vector<Channel> channels,
                             std::string label = "main") {
    CollectionSpec s;
    s.kind = std::string(kind);
    s.counts = counts;
    s.channels = std::move(channels);
    s.label = std::move(label);
    return s;
  }
  // A collection at one place in the plan.
  static CollectionSpec at(std::string_view kind, int counts, std::vector<Channel> channels, std::string gas,
                           int segment, int position) {
    auto s = spec(kind, counts, std::move(channels));
    s.gas = std::move(gas);
    s.segment = segment;
    s.position = position;
    return s;
  }
  static plan::Fits average() {
    plan::Fits f;
    f.signal["default"] = reduction::FitKind::Average;
    return f;
  }

  TimePoint t0_ = TimePoint{} + 100s;
  ManualClock clock_{t0_};
  SignalBus bus_;
  Collector collector_{clock_, &bus_};
};

TEST_F(CollectorTest, KeysSeriesByIsotopeDetectorAndKind) {
  collector_.begin(spec(kSignal, 2, {{"Ar40", "H1"}, {"Ar36", "CDD"}}));
  EXPECT_EQ(collector_.add(reading(t0_ + 1s, {{"H1", 100}, {"CDD", 3}, {"AX", 9}})), CollectStatus::Running);
  EXPECT_EQ(collector_.add(reading(t0_ + 2s, {{"H1", 110}, {"CDD", 4}})), CollectStatus::Complete);
  EXPECT_EQ(collector_.finish(), 2);

  // Peak hop: Ar36 again, now as a baseline hop on CDD; Ar39 on CDD.
  collector_.begin(spec(kSignal, 1, {{"Ar39", "CDD"}}));
  collector_.add(reading(t0_ + 3s, {{"CDD", 50}}));
  collector_.begin(spec(kBaseline, 1, {{"Ar36", "CDD"}}));
  collector_.add(reading(t0_ + 4s, {{"CDD", 0.1}}));

  auto d = collector_.data();
  ASSERT_EQ(d.series.size(), 4u);  // AX is not a channel and gets no series
  const auto& ar40 = d.series.at(skey("Ar40", "H1", kSignal));
  EXPECT_EQ(ar40.v, (std::vector<double>{100, 110}));
  EXPECT_EQ(ar40.t, (std::vector<double>{1, 2}));
  EXPECT_TRUE(ar40.sigma.empty());
  EXPECT_EQ(d.series.at(skey("Ar36", "CDD", kSignal)).v.size(), 2u);
  EXPECT_EQ(d.series.at(skey("Ar36", "CDD", kBaseline)).v, (std::vector<double>{0.1}));
  EXPECT_EQ(d.series.at(skey("Ar39", "CDD", kSignal)).v, (std::vector<double>{50}));
  EXPECT_EQ(to_string(skey("Ar36", "CDD", kBaseline)), "Ar36@CDD:baseline::0.0");
}

TEST_F(CollectorTest, MissingDetectorValueStillCountsTheReading) {
  collector_.begin(spec(kSignal, 2, {{"Ar40", "H1"}}));
  auto r = reading(t0_ + 1s, {});
  r.values["H1"] = std::nullopt;
  EXPECT_EQ(collector_.add(r), CollectStatus::Running);
  EXPECT_EQ(collector_.add(reading(t0_ + 2s, {{"H1", 5}})), CollectStatus::Complete);
  EXPECT_EQ(collector_.series(skey("Ar40", "H1", kSignal))->v.size(), 1u);
  EXPECT_EQ(collector_.data().counts.at("main"), 2);
}

TEST_F(CollectorTest, SigmaStaysParallelOnceAnyPointCarriesOne) {
  collector_.begin(spec(kSignal, 3, {{"Ar40", "H1"}}));
  collector_.add(reading(t0_ + 1s, {{"H1", 1}}));
  auto r = reading(t0_ + 2s, {{"H1", 2}});
  r.values["H1"]->sigma = 0.5;
  collector_.add(r);
  collector_.add(reading(t0_ + 3s, {{"H1", 3}}));
  auto s = *collector_.series(skey("Ar40", "H1", kSignal));
  EXPECT_EQ(s.sigma, (std::vector<double>{0, 0.5, 0}));
}

TEST_F(CollectorTest, SetTargetChangesTheRunningCollection) {
  collector_.begin(spec(kSignal, 10, {{"Ar40", "H1"}}));
  collector_.add(reading(t0_ + 1s, {{"H1", 1}}));
  collector_.set_target(2);
  EXPECT_EQ(collector_.target(), 2);
  EXPECT_EQ(collector_.add(reading(t0_ + 2s, {{"H1", 1}})), CollectStatus::Complete);
  collector_.set_target(0);  // clamped to 1
  EXPECT_EQ(collector_.target(), 1);
}

TEST_F(CollectorTest, TruncateEndsAtTheNextReadingAndBeginClearsIt) {
  collector_.begin(spec(kSignal, 10, {{"Ar40", "H1"}}));
  collector_.add(reading(t0_ + 1s, {{"H1", 1}}));
  collector_.truncate();
  EXPECT_EQ(collector_.add(reading(t0_ + 2s, {{"H1", 1}})), CollectStatus::Truncated);
  EXPECT_TRUE(collector_.truncated());
  collector_.begin(spec(kBaseline, 2, {{"", "H1"}}, "baseline.after"));
  EXPECT_FALSE(collector_.truncated());
  EXPECT_EQ(collector_.add(reading(t0_ + 3s, {{"H1", 1}})), CollectStatus::Running);
}

TEST_F(CollectorTest, ReadingHookCanEndTheCollection) {
  std::vector<int> seen;
  collector_.begin(spec(kSignal, 10, {{"Ar40", "H1"}}), [&](int count, double) {
    seen.push_back(count);
    return count == 3;
  });
  EXPECT_EQ(collector_.add(reading(t0_ + 1s, {{"H1", 1}})), CollectStatus::Running);
  EXPECT_EQ(collector_.add(reading(t0_ + 2s, {{"H1", 1}})), CollectStatus::Running);
  EXPECT_EQ(collector_.add(reading(t0_ + 3s, {{"H1", 1}})), CollectStatus::Truncated);
  EXPECT_EQ(seen, (std::vector<int>{1, 2, 3}));
}

TEST_F(CollectorTest, PublishesSeriesUpdatedPerReading) {
  std::vector<SeriesUpdated> events;
  auto sub = bus_.subscribe<SeriesUpdated>([&](const SeriesUpdated& e) { events.push_back(e); });
  collector_.begin(spec(kSniff, 2, {{"Ar40", "H1"}, {"Ar39", "AX"}}, "sniff"));
  collector_.add(reading(t0_ + 1s, {{"H1", 7}, {"AX", 8}}));
  collector_.add(reading(t0_ + 2s, {{"H1", 9}}));
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0].label, "sniff");
  EXPECT_EQ(events[0].kind, kSniff);
  EXPECT_EQ(events[0].count, 1);
  EXPECT_EQ(events[0].target, 2);
  EXPECT_EQ(events[0].values.size(), 2u);
  EXPECT_EQ(events[1].values.size(), 1u);
  EXPECT_DOUBLE_EQ(events[1].t, 2.0);
}

TEST_F(CollectorTest, FitSeriesIsRelativeToTimeZero) {
  collector_.begin(spec(kSignal, 2, {{"Ar40", "H1"}}));
  collector_.add(reading(t0_ + 20s, {{"H1", 1}}));
  collector_.add(reading(t0_ + 21s, {{"H1", 2}}));
  EXPECT_EQ(collector_.fit_series(skey("Ar40", "H1", kSignal))->x, (std::vector<double>{20, 21}));
  collector_.set_time_zero(18);
  auto s = *collector_.fit_series(skey("Ar40", "H1", kSignal));
  EXPECT_EQ(s.x, (std::vector<double>{2, 3}));
  EXPECT_EQ(s.y, (std::vector<double>{1, 2}));
  EXPECT_FALSE(collector_.fit_series(skey("Ar39", "AX", kSignal)));
}

TEST_F(CollectorTest, MetricsDriveConditionals) {
  collector_.begin(spec(kBaseline, 2, {{"", "H1"}}, "baseline.before"));
  collector_.add(reading(t0_ + 1s, {{"H1", 1}}));
  collector_.add(reading(t0_ + 2s, {{"H1", 3}}));  // H1 baseline mean 2
  collector_.set_time_zero(5);
  collector_.begin(spec(kSignal, 3, {{"Ar40", "H1"}, {"Ar39", "AX"}}));
  // Linear in x = t - time zero (1, 2, 3): Ar40 = 100 + 50x, Ar39 = 10 + 5x.
  collector_.add(reading(t0_ + 6s, {{"H1", 150}, {"AX", 15}}));
  collector_.add(reading(t0_ + 7s, {{"H1", 200}, {"AX", 20}}));
  collector_.add(reading(t0_ + 8s, {{"H1", 250}, {"AX", 25}}));
  clock_.set(t0_ + 9s);

  const auto& m = collector_.metrics();
  Variables vars;
  auto value = [&](const std::string& text) {
    auto e = parse_expression(text);
    EXPECT_TRUE(e) << text;
    auto r = evaluate(**e, m, vars);
    EXPECT_TRUE(r) << text << ": " << (r ? "" : r.error().what);
    return r ? *r : -1e300;
  };
  EXPECT_NEAR(value("Ar40.intercept"), 100, 1e-9);
  EXPECT_NEAR(value("Ar40.std_dev"), 0, 1e-9);  // a perfect line
  EXPECT_NEAR(value("Ar40.bs_corrected"), 98, 1e-9);
  EXPECT_NEAR(value("Ar40"), 98, 1e-9);         // icfactor 1
  EXPECT_NEAR(value("Ar39"), 10, 1e-9);         // AX has no baseline
  EXPECT_NEAR(value("Ar40/Ar39"), 9.8, 1e-9);
  EXPECT_EQ(value("Ar40.cur"), 250);
  EXPECT_EQ(value("average(Ar40.bs)"), 2);
  EXPECT_EQ(value("H1.intensity"), 250);
  EXPECT_EQ(value("count(Ar40)"), 3);
  EXPECT_EQ(value("slope(Ar40)"), 50);           // raw points, per reading
  EXPECT_EQ(value("max(Ar40.bs_corrected)"), 248);
  EXPECT_EQ(value("elapsed()"), 4);
  EXPECT_FALSE(m.scalar(MetricRef{MetricRef::Kind::Gauge, "ion_pump", "", "pressure", "", ""}));
  EXPECT_FALSE(m.scalar(MetricRef{MetricRef::Kind::Computed, "age", "", "", "", ""}));  // no constants

  collector_.set_icfactors({{"H1", 2.0}});
  EXPECT_NEAR(value("Ar40"), 196, 1e-9);
  EXPECT_NEAR(value("Ar40.ic_corrected"), 196, 1e-9);
  EXPECT_NEAR(value("Ar40.bs_corrected"), 98, 1e-9);

  // Unknown metrics go to the fallback context.
  MapContext fallback;
  fallback.series_data["gauge.ion_pump.pressure"] = {2e-6};
  collector_.set_fallback(&fallback);
  EXPECT_EQ(value("gauge.ion_pump.pressure > 1e-6"), 1);
}

TEST_F(CollectorTest, FitFollowsThePlanAndFallsBackToAverage) {
  collector_.set_time_zero(0);
  collector_.begin(spec(kSignal, 3, {{"Ar40", "H1"}}));
  collector_.add(reading(t0_ + 1s, {{"H1", 10}}));
  EXPECT_NEAR(*collector_.metrics().scalar(MetricRef{MetricRef::Kind::Isotope, "Ar40", "", "", "", ""}), 10, 1e-12);  // 1 point
  collector_.add(reading(t0_ + 2s, {{"H1", 20}}));
  collector_.add(reading(t0_ + 3s, {{"H1", 30}}));
  EXPECT_NEAR(collector_.intercept("Ar40")->value, 0, 1e-9);  // linear default
  plan::Fits fits;
  fits.signal["Ar40"] = reduction::FitKind::Average;
  collector_.set_fits(fits);
  EXPECT_NEAR(collector_.intercept("Ar40")->value, 20, 1e-9);
  EXPECT_FALSE(collector_.intercept("Ar39"));
}

TEST_F(CollectorTest, ComputedArArValues) {
  plan::Fits fits;
  fits.signal["default"] = reduction::FitKind::Average;
  collector_.set_fits(fits);
  reduction::ArArConstants c;
  c.j = 0.01;
  collector_.set_arar(c);
  collector_.set_time_zero(0);
  collector_.begin(spec(kSignal, 2, {{"Ar40", "H1"}, {"Ar39", "AX"}, {"Ar36", "CDD"}}));
  collector_.add(reading(t0_ + 1s, {{"H1", 1000}, {"AX", 100}, {"CDD", 1}}));
  collector_.add(reading(t0_ + 2s, {{"H1", 1000}, {"AX", 100}, {"CDD", 1}}));
  const auto& m = collector_.metrics();
  const auto expected = reduction::compute_arar({1.0, std::nullopt, std::nullopt, 100.0, 1000.0}, c);
  for (const char* name : {"radiogenic_yield", "rad40", "atm40", "age"}) {
    auto v = m.scalar(MetricRef{MetricRef::Kind::Computed, name, "", "", "", ""});
    ASSERT_TRUE(v) << name;
    EXPECT_DOUBLE_EQ(*v, expected.at(name)) << name;
  }
  EXPECT_DOUBLE_EQ(*m.scalar(MetricRef{MetricRef::Kind::Computed, "instant_age", "", "", "", ""}), expected.at("age"));
  EXPECT_FALSE(m.scalar(MetricRef{MetricRef::Kind::Computed, "kca", "", "", "", ""}));  // no Ar37
}

TEST_F(CollectorTest, IsotopeOnTwoDetectorsResolvesToTheLatest) {
  collector_.begin(spec(kSignal, 1, {{"Ar36", "CDD"}}));
  collector_.add(reading(t0_ + 1s, {{"CDD", 1}}));
  collector_.begin(spec(kSignal, 1, {{"Ar36", "L2"}}));
  collector_.add(reading(t0_ + 2s, {{"L2", 2}}));
  EXPECT_EQ(*collector_.metrics().scalar(MetricRef{MetricRef::Kind::Isotope, "Ar36", "", "", "", ""}), 2);
}

TEST_F(CollectorTest, TripsAndTimingAreRecorded) {
  collector_.set_inlet_open(3);
  collector_.set_inlet_close(18);
  Trip trip;
  trip.name = "t";
  trip.value = 9e5;
  trip.count = 1;
  trip.ts = 2.0;
  collector_.add_trips({trip});
  auto d = collector_.data();
  EXPECT_EQ(d.timing.epoch, t0_);
  EXPECT_EQ(*d.timing.inlet_open, 3);
  EXPECT_EQ(*d.timing.inlet_close, 18);
  ASSERT_EQ(d.trips.size(), 1u);
  EXPECT_EQ(d.trips[0].name, "t");
}

}  // namespace

namespace {

TEST_F(CollectorTest, PublishesTheLiveFitOfEachTouchedSeries) {
  std::vector<FitsUpdated> fits;
  auto sub = bus_.subscribe<FitsUpdated>([&](const FitsUpdated& e) { fits.push_back(e); });
  plan::Fits f;
  f.signal = {{"default", reduction::FitKind::Linear}};
  collector_.set_fits(f);
  collector_.set_time_zero(10.0);

  // Sniff readings are not fitted.
  collector_.begin(spec(kSniff, 1, {{"Ar40", "H1"}}, "sniff"));
  collector_.add(reading(t0_ + 1s, {{"H1", 1}}));
  EXPECT_TRUE(fits.empty());

  // One point: averaged until the linear fit has enough.
  collector_.begin(spec(kSignal, 3, {{"Ar40", "H1"}, {"Ar36", "CDD"}}));
  collector_.add(reading(t0_ + 11s, {{"H1", 102}, {"CDD", 5}}));
  ASSERT_EQ(fits.size(), 1u);
  EXPECT_EQ(fits[0].label, "main");
  EXPECT_DOUBLE_EQ(fits[0].time_zero, 10.0);
  ASSERT_EQ(fits[0].fits.size(), 2u);
  EXPECT_EQ(fits[0].fits[0].fit.kind, reduction::FitKind::Average);

  // y = 100 + 2 x with x = t - time zero: the intercept is at time zero.
  collector_.add(reading(t0_ + 12s, {{"H1", 104}, {"CDD", 5}}));
  collector_.add(reading(t0_ + 13s, {{"H1", 106}}));
  ASSERT_EQ(fits.size(), 3u);
  ASSERT_EQ(fits[2].fits.size(), 1u);  // CDD had no value in the last reading
  const auto& ar40 = fits[2].fits[0];
  EXPECT_EQ(ar40.key, (skey("Ar40", "H1", kSignal)));
  EXPECT_EQ(ar40.fit.kind, reduction::FitKind::Linear);
  EXPECT_NEAR(ar40.fit.value, 100.0, 1e-9);
  EXPECT_NEAR(reduction::predict(ar40.fit, 3.0), 106.0, 1e-9);
  // The same number the conditionals see.
  EXPECT_NEAR(collector_.intercept("Ar40")->value, ar40.fit.value, 1e-12);

  // Baselines use the plan's baseline fit (average by default).
  collector_.begin(spec(kBaseline, 1, {{"", "H1"}}, "baseline.after"));
  collector_.add(reading(t0_ + 20s, {{"H1", 0.25}}));
  ASSERT_EQ(fits.size(), 4u);
  EXPECT_EQ(fits[3].kind, kBaseline);
  EXPECT_EQ(fits[3].fits[0].fit.kind, reduction::FitKind::Average);
  EXPECT_DOUBLE_EQ(fits[3].fits[0].fit.value, 0.25);
}

TEST_F(CollectorTest, KeysCarryGasSegmentPosition) {
  std::vector<SeriesUpdated> events;
  auto sub = bus_.subscribe<SeriesUpdated>([&](const SeriesUpdated& e) { events.push_back(e); });
  collector_.begin(at(kSignal, 1, {{"Ar40", "H1"}}, "argon", 2, 1));
  collector_.add(reading(t0_ + 1s, {{"H1", 100}}));
  collector_.begin(at(kSignal, 1, {{"Ar40", "H1"}}, "argon", 2, 3));
  collector_.add(reading(t0_ + 2s, {{"H1", 101}}));

  const SeriesKey first{"Ar40", "H1", std::string(kSignal), "argon", 2, 1};
  const SeriesKey second{"Ar40", "H1", std::string(kSignal), "argon", 2, 3};
  const auto d = collector_.data();
  ASSERT_EQ(d.series.size(), 2u);  // one position, one series
  EXPECT_EQ(d.series.at(first).v, (std::vector<double>{100}));
  EXPECT_EQ(d.series.at(second).v, (std::vector<double>{101}));
  EXPECT_EQ(to_string(first), "Ar40@H1:signal:argon:2.1");
  ASSERT_EQ(events.size(), 2u);
  ASSERT_EQ(events[1].values.size(), 1u);
  EXPECT_EQ(events[1].values[0].first, second);
}

TEST_F(CollectorTest, InterceptByLabelPicksMostRecentDetector) {
  collector_.set_fits(average());
  collector_.begin(spec(kSignal, 2, {{"Ar40", "H1"}}));
  collector_.add(reading(t0_ + 1s, {{"H1", 100}}));
  collector_.add(reading(t0_ + 2s, {{"H1", 100}}));
  collector_.begin(spec(kSignal, 2, {{"Ar40", "H2"}}));
  collector_.add(reading(t0_ + 3s, {{"H2", 50}}));
  collector_.add(reading(t0_ + 4s, {{"H2", 50}}));
  EXPECT_DOUBLE_EQ(collector_.intercept("Ar40")->value, 50);
  collector_.begin(spec(kSignal, 1, {{"Ar40", "H1"}}));
  collector_.add(reading(t0_ + 5s, {{"H1", 100}}));
  EXPECT_DOUBLE_EQ(collector_.intercept("Ar40")->value, 100);
}

// Worked plan 7.3: one signal segment, Ar40 hopped from cup to cup.
TEST_F(CollectorTest, InterceptByQualifiedName) {
  collector_.set_fits(average());
  const std::vector<std::pair<std::string, double>> hops{{"H2", 20}, {"H1", 10}};
  int t = 0;
  for (int cycle = 0; cycle < 2; ++cycle) {
    for (std::size_t pos = 0; pos < hops.size(); ++pos) {
      const auto& [det, v] = hops[pos];
      collector_.begin(at(kSignal, 1, {{"Ar40", det}}, "", 0, static_cast<int>(pos)));
      collector_.add(reading(t0_ + std::chrono::seconds(++t), {{det, v + cycle}}));
    }
  }
  EXPECT_DOUBLE_EQ(collector_.intercept("Ar40@H2")->value, 20.5);
  EXPECT_DOUBLE_EQ(collector_.intercept("Ar40@H1")->value, 10.5);
  EXPECT_DOUBLE_EQ(collector_.intercept("Ar40")->value, 10.5);  // H1 measured last
  EXPECT_FALSE(collector_.intercept("Ar40@AX"));
  EXPECT_FALSE(collector_.intercept("Ar39@H1"));
}

// "Ar40@H2.cur" reads H2's points live; the bare name the most recent detector's.
TEST_F(CollectorTest, DetectorQualifiedMetrics) {
  collector_.set_fits(average());
  const std::vector<std::pair<std::string, double>> hops{{"H2", 20}, {"H1", 10}};
  int t = 0;
  for (int cycle = 0; cycle < 2; ++cycle) {
    for (std::size_t pos = 0; pos < hops.size(); ++pos) {
      const auto& [det, v] = hops[pos];
      collector_.begin(at(kSignal, 1, {{"Ar40", det}}, "", 0, static_cast<int>(pos)));
      collector_.add(reading(t0_ + std::chrono::seconds(++t), {{det, v + cycle}}));
    }
  }
  const auto& m = collector_.metrics();
  Variables vars;
  auto value = [&](const std::string& text) {
    auto e = parse_expression(text);
    EXPECT_TRUE(e) << text;
    auto r = evaluate(**e, m, vars);
    EXPECT_TRUE(r) << text << ": " << (r ? "" : r.error().what);
    return r ? *r : -1e300;
  };
  EXPECT_EQ(value("Ar40@H2.cur"), 21);
  EXPECT_EQ(value("Ar40.cur"), 11);
  EXPECT_DOUBLE_EQ(value("Ar40@H2.intercept"), 20.5);
  EXPECT_EQ(value("count(Ar40@H2)"), 2);
  EXPECT_DOUBLE_EQ(value("Ar40@H2/Ar40@H1"), 20.5 / 10.5);
}

TEST_F(CollectorTest, OneLabelOneDetectorTwoPositionsFitsAsOneSeries) {
  collector_.set_fits(average());
  // Interleaved: position 0 at 1 s and 3 s, position 1 at 2 s and 4 s.
  for (int i = 0; i < 4; ++i) {
    const int pos = i % 2;
    collector_.begin(at(kSignal, 1, {{"Ar40", "H1"}}, "", 0, pos));
    collector_.add(reading(t0_ + std::chrono::seconds(i + 1), {{"H1", pos == 0 ? 10.0 : 30.0}}));
  }
  EXPECT_EQ(collector_.data().series.size(), 2u);
  EXPECT_DOUBLE_EQ(collector_.intercept("Ar40")->value, 20);  // neither position's 10 or 30
  EXPECT_DOUBLE_EQ(collector_.intercept("Ar40@H1")->value, 20);
  const auto merged = collector_.series_of("Ar40", kSignal);
  ASSERT_TRUE(merged);
  EXPECT_EQ(merged->t, (std::vector<double>{1, 2, 3, 4}));
  EXPECT_EQ(merged->v, (std::vector<double>{10, 30, 10, 30}));
  EXPECT_EQ(merged->saturated.size(), 4u);
  EXPECT_EQ(collector_.metrics().series(MetricRef{MetricRef::Kind::Isotope, "Ar40", "", "", "", ""})->size(), 4u);
}

}  // namespace
