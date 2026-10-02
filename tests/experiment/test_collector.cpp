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

class CollectorTest : public ::testing::Test {
 protected:
  void SetUp() override { collector_.start(t0_); }

  CollectionSpec spec(SeriesKind kind, int counts, std::vector<Channel> channels, std::string label = "main") {
    return CollectionSpec{kind, counts, std::move(channels), std::move(label)};
  }

  TimePoint t0_ = TimePoint{} + 100s;
  ManualClock clock_{t0_};
  SignalBus bus_;
  Collector collector_{clock_, &bus_};
};

TEST_F(CollectorTest, KeysSeriesByIsotopeDetectorAndKind) {
  collector_.begin(spec(SeriesKind::Signal, 2, {{"Ar40", "H1"}, {"Ar36", "CDD"}}));
  EXPECT_EQ(collector_.add(reading(t0_ + 1s, {{"H1", 100}, {"CDD", 3}, {"AX", 9}})), CollectStatus::Running);
  EXPECT_EQ(collector_.add(reading(t0_ + 2s, {{"H1", 110}, {"CDD", 4}})), CollectStatus::Complete);
  EXPECT_EQ(collector_.finish(), 2);

  // Peak hop: Ar36 again, now as a baseline hop on CDD; Ar39 on CDD.
  collector_.begin(spec(SeriesKind::Signal, 1, {{"Ar39", "CDD"}}));
  collector_.add(reading(t0_ + 3s, {{"CDD", 50}}));
  collector_.begin(spec(SeriesKind::Baseline, 1, {{"Ar36", "CDD"}}));
  collector_.add(reading(t0_ + 4s, {{"CDD", 0.1}}));

  auto d = collector_.data();
  ASSERT_EQ(d.series.size(), 4u);  // AX is not a channel and gets no series
  const auto& ar40 = d.series.at({"Ar40", "H1", SeriesKind::Signal});
  EXPECT_EQ(ar40.v, (std::vector<double>{100, 110}));
  EXPECT_EQ(ar40.t, (std::vector<double>{1, 2}));
  EXPECT_TRUE(ar40.sigma.empty());
  EXPECT_EQ(d.series.at({"Ar36", "CDD", SeriesKind::Signal}).v.size(), 2u);
  EXPECT_EQ(d.series.at({"Ar36", "CDD", SeriesKind::Baseline}).v, (std::vector<double>{0.1}));
  EXPECT_EQ(d.series.at({"Ar39", "CDD", SeriesKind::Signal}).v, (std::vector<double>{50}));
  EXPECT_EQ(to_string(SeriesKey{"Ar36", "CDD", SeriesKind::Baseline}), "Ar36:CDD:baseline");
}

TEST_F(CollectorTest, MissingDetectorValueStillCountsTheReading) {
  collector_.begin(spec(SeriesKind::Signal, 2, {{"Ar40", "H1"}}));
  auto r = reading(t0_ + 1s, {});
  r.values["H1"] = std::nullopt;
  EXPECT_EQ(collector_.add(r), CollectStatus::Running);
  EXPECT_EQ(collector_.add(reading(t0_ + 2s, {{"H1", 5}})), CollectStatus::Complete);
  EXPECT_EQ(collector_.series({"Ar40", "H1", SeriesKind::Signal})->v.size(), 1u);
  EXPECT_EQ(collector_.data().counts.at("main"), 2);
}

TEST_F(CollectorTest, SigmaStaysParallelOnceAnyPointCarriesOne) {
  collector_.begin(spec(SeriesKind::Signal, 3, {{"Ar40", "H1"}}));
  collector_.add(reading(t0_ + 1s, {{"H1", 1}}));
  auto r = reading(t0_ + 2s, {{"H1", 2}});
  r.values["H1"]->sigma = 0.5;
  collector_.add(r);
  collector_.add(reading(t0_ + 3s, {{"H1", 3}}));
  auto s = *collector_.series({"Ar40", "H1", SeriesKind::Signal});
  EXPECT_EQ(s.sigma, (std::vector<double>{0, 0.5, 0}));
}

TEST_F(CollectorTest, SetTargetChangesTheRunningCollection) {
  collector_.begin(spec(SeriesKind::Signal, 10, {{"Ar40", "H1"}}));
  collector_.add(reading(t0_ + 1s, {{"H1", 1}}));
  collector_.set_target(2);
  EXPECT_EQ(collector_.target(), 2);
  EXPECT_EQ(collector_.add(reading(t0_ + 2s, {{"H1", 1}})), CollectStatus::Complete);
  collector_.set_target(0);  // clamped to 1
  EXPECT_EQ(collector_.target(), 1);
}

TEST_F(CollectorTest, TruncateEndsAtTheNextReadingAndBeginClearsIt) {
  collector_.begin(spec(SeriesKind::Signal, 10, {{"Ar40", "H1"}}));
  collector_.add(reading(t0_ + 1s, {{"H1", 1}}));
  collector_.truncate();
  EXPECT_EQ(collector_.add(reading(t0_ + 2s, {{"H1", 1}})), CollectStatus::Truncated);
  EXPECT_TRUE(collector_.truncated());
  collector_.begin(spec(SeriesKind::Baseline, 2, {{"", "H1"}}, "baseline.after"));
  EXPECT_FALSE(collector_.truncated());
  EXPECT_EQ(collector_.add(reading(t0_ + 3s, {{"H1", 1}})), CollectStatus::Running);
}

TEST_F(CollectorTest, ReadingHookCanEndTheCollection) {
  std::vector<int> seen;
  collector_.begin(spec(SeriesKind::Signal, 10, {{"Ar40", "H1"}}), [&](int count, double) {
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
  collector_.begin(spec(SeriesKind::Sniff, 2, {{"Ar40", "H1"}, {"Ar39", "AX"}}, "sniff"));
  collector_.add(reading(t0_ + 1s, {{"H1", 7}, {"AX", 8}}));
  collector_.add(reading(t0_ + 2s, {{"H1", 9}}));
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0].label, "sniff");
  EXPECT_EQ(events[0].kind, SeriesKind::Sniff);
  EXPECT_EQ(events[0].count, 1);
  EXPECT_EQ(events[0].target, 2);
  EXPECT_EQ(events[0].values.size(), 2u);
  EXPECT_EQ(events[1].values.size(), 1u);
  EXPECT_DOUBLE_EQ(events[1].t, 2.0);
}

TEST_F(CollectorTest, FitSeriesIsRelativeToTimeZero) {
  collector_.begin(spec(SeriesKind::Signal, 2, {{"Ar40", "H1"}}));
  collector_.add(reading(t0_ + 20s, {{"H1", 1}}));
  collector_.add(reading(t0_ + 21s, {{"H1", 2}}));
  EXPECT_EQ(collector_.fit_series({"Ar40", "H1", SeriesKind::Signal})->x, (std::vector<double>{20, 21}));
  collector_.set_time_zero(18);
  auto s = *collector_.fit_series({"Ar40", "H1", SeriesKind::Signal});
  EXPECT_EQ(s.x, (std::vector<double>{2, 3}));
  EXPECT_EQ(s.y, (std::vector<double>{1, 2}));
  EXPECT_FALSE(collector_.fit_series({"Ar39", "AX", SeriesKind::Signal}));
}

TEST_F(CollectorTest, MetricsDriveConditionals) {
  collector_.begin(spec(SeriesKind::Baseline, 2, {{"", "H1"}}, "baseline.before"));
  collector_.add(reading(t0_ + 1s, {{"H1", 1}}));
  collector_.add(reading(t0_ + 2s, {{"H1", 3}}));  // H1 baseline mean 2
  collector_.set_time_zero(5);
  collector_.begin(spec(SeriesKind::Signal, 3, {{"Ar40", "H1"}, {"Ar39", "AX"}}));
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
  EXPECT_FALSE(m.scalar(MetricRef{MetricRef::Kind::Gauge, "ion_pump", "", "pressure"}));
  EXPECT_FALSE(m.scalar(MetricRef{MetricRef::Kind::Computed, "age", "", ""}));  // no constants

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
  collector_.begin(spec(SeriesKind::Signal, 3, {{"Ar40", "H1"}}));
  collector_.add(reading(t0_ + 1s, {{"H1", 10}}));
  EXPECT_NEAR(*collector_.metrics().scalar(MetricRef{MetricRef::Kind::Isotope, "Ar40", "", ""}), 10, 1e-12);  // 1 point
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
  collector_.begin(spec(SeriesKind::Signal, 2, {{"Ar40", "H1"}, {"Ar39", "AX"}, {"Ar36", "CDD"}}));
  collector_.add(reading(t0_ + 1s, {{"H1", 1000}, {"AX", 100}, {"CDD", 1}}));
  collector_.add(reading(t0_ + 2s, {{"H1", 1000}, {"AX", 100}, {"CDD", 1}}));
  const auto& m = collector_.metrics();
  const auto expected = reduction::compute_arar({1.0, std::nullopt, std::nullopt, 100.0, 1000.0}, c);
  for (const char* name : {"radiogenic_yield", "rad40", "atm40", "age"}) {
    auto v = m.scalar(MetricRef{MetricRef::Kind::Computed, name, "", ""});
    ASSERT_TRUE(v) << name;
    EXPECT_DOUBLE_EQ(*v, expected.at(name)) << name;
  }
  EXPECT_DOUBLE_EQ(*m.scalar(MetricRef{MetricRef::Kind::Computed, "instant_age", "", ""}), expected.at("age"));
  EXPECT_FALSE(m.scalar(MetricRef{MetricRef::Kind::Computed, "kca", "", ""}));  // no Ar37
}

TEST_F(CollectorTest, IsotopeOnTwoDetectorsResolvesToTheLatest) {
  collector_.begin(spec(SeriesKind::Signal, 1, {{"Ar36", "CDD"}}));
  collector_.add(reading(t0_ + 1s, {{"CDD", 1}}));
  collector_.begin(spec(SeriesKind::Signal, 1, {{"Ar36", "L2"}}));
  collector_.add(reading(t0_ + 2s, {{"L2", 2}}));
  EXPECT_EQ(*collector_.metrics().scalar(MetricRef{MetricRef::Kind::Isotope, "Ar36", "", ""}), 2);
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
  collector_.begin(spec(SeriesKind::Sniff, 1, {{"Ar40", "H1"}}, "sniff"));
  collector_.add(reading(t0_ + 1s, {{"H1", 1}}));
  EXPECT_TRUE(fits.empty());

  // One point: averaged until the linear fit has enough.
  collector_.begin(spec(SeriesKind::Signal, 3, {{"Ar40", "H1"}, {"Ar36", "CDD"}}));
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
  EXPECT_EQ(ar40.key, (SeriesKey{"Ar40", "H1", SeriesKind::Signal}));
  EXPECT_EQ(ar40.fit.kind, reduction::FitKind::Linear);
  EXPECT_NEAR(ar40.fit.value, 100.0, 1e-9);
  EXPECT_NEAR(reduction::predict(ar40.fit, 3.0), 106.0, 1e-9);
  // The same number the conditionals see.
  EXPECT_NEAR(collector_.intercept("Ar40")->value, ar40.fit.value, 1e-12);

  // Baselines use the plan's baseline fit (average by default).
  collector_.begin(spec(SeriesKind::Baseline, 1, {{"", "H1"}}, "baseline.after"));
  collector_.add(reading(t0_ + 20s, {{"H1", 0.25}}));
  ASSERT_EQ(fits.size(), 4u);
  EXPECT_EQ(fits[3].kind, SeriesKind::Baseline);
  EXPECT_EQ(fits[3].fits[0].fit.kind, reduction::FitKind::Average);
  EXPECT_DOUBLE_EQ(fits[3].fits[0].fit.value, 0.25);
}

}  // namespace
