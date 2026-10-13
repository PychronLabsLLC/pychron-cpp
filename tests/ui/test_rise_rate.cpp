// The rise rate calculation and a run of readings: pure data, no widgets.

#include <chrono>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QtTest/QtTest>

#include "rise_rate.hpp"

using namespace pychron;
using pychron::spectrometer::IntensityReading;
using pychron::spectrometer::Value;
using pychron::ui::rise_rate;
using pychron::ui::RisePoint;
using pychron::ui::RiseRun;

namespace {

using Cell = std::pair<std::string, std::optional<double>>;

TimePoint at(double t_s) {
  return TimePoint{} + std::chrono::duration_cast<Duration>(std::chrono::duration<double>(t_s));
}

IntensityReading row(double t_s, std::initializer_list<Cell> cells, bool saturated = false) {
  IntensityReading r;
  r.reading.ts = at(t_s);
  for (const auto& [name, v] : cells) {
    if (v) {
      Value val;
      val.mean = *v;
      val.saturated = saturated;
      r.reading.values[name] = val;
    } else {
      r.reading.values[name] = std::nullopt;
    }
  }
  return r;
}

bool near(double a, double b, double tolerance = 1e-9) { return std::abs(a - b) <= tolerance; }

}  // namespace

class TestRiseRate : public QObject {
  Q_OBJECT

 private slots:
  void endpointsAndSlopeByHand() {
    const std::vector<RisePoint> points{{0, 1}, {60, 3}, {120, 5}};
    const auto r = rise_rate(points);
    QVERIFY(r);
    QCOMPARE(r->n, 3);
    QVERIFY(near(r->duration_min, 2.0));
    QVERIFY(near(r->rise, 4.0));
    QVERIFY(near(r->endpoints, 2.0));
    QVERIFY(near(r->linear, 2.0));
    QVERIFY(r->linear_se);
    QVERIFY(near(*r->linear_se, 0.0));
  }

  // x 0, 1, 2, 3 min; y 0, 1.1, 1.9, 3.1: Sxx 5, Sxy 5.05, slope 1.01,
  // residuals -0.01, 0.08, -0.13, 0.06, SSR 0.027, se sqrt(0.027 / 2 / 5).
  void aNoisyLineHasAStandardError() {
    const std::vector<RisePoint> points{{0, 0}, {60, 1.1}, {120, 1.9}, {180, 3.1}};
    const auto r = rise_rate(points);
    QVERIFY(r);
    QVERIFY(near(r->linear, 1.01));
    QVERIFY(r->linear_se);
    QVERIFY(near(*r->linear_se, std::sqrt(0.0027)));
    QVERIFY(near(r->endpoints, 3.1 / 3.0));
  }

  // A reading that comes late. A fit that took the samples as evenly spaced
  // (legacy's) gives 1.82 here, not 2.
  void unevenSpacingGivesTheTrueSlope() {
    std::vector<RisePoint> points;
    for (const double t : {0.0, 10.0, 20.0, 300.0}) points.push_back({t, 2.0 * t / 60.0});
    const auto r = rise_rate(points);
    QVERIFY(r);
    QVERIFY(near(r->linear, 2.0));
    QVERIFY(near(r->endpoints, 2.0));
  }

  void nonFiniteValuesAreLeftOut() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::vector<RisePoint> points{{0, nan}, {60, 3}, {90, nan}, {120, 5}, {180, 7}, {240, nan}};
    const auto r = rise_rate(points);
    QVERIFY(r);
    QCOMPARE(r->n, 3);
    QVERIFY(near(r->duration_min, 2.0));
    QVERIFY(near(r->rise, 4.0));
    QVERIFY(near(r->linear, 2.0));
  }

  void onePointIsNoRate() {
    QVERIFY(!rise_rate(std::vector<RisePoint>{}));
    QVERIFY(!rise_rate(std::vector<RisePoint>{{5, 1}}));
    const double nan = std::numeric_limits<double>::quiet_NaN();
    QVERIFY(!rise_rate(std::vector<RisePoint>{{5, 1}, {6, nan}}));
  }

  void zeroDurationIsNoRate() { QVERIFY(!rise_rate(std::vector<RisePoint>{{5, 1}, {5, 2}})); }

  void twoPointsHaveNoStandardError() {
    const auto r = rise_rate(std::vector<RisePoint>{{0, 1}, {30, 2}});
    QVERIFY(r);
    QCOMPARE(r->n, 2);
    QVERIFY(near(r->linear, 2.0));
    QVERIFY(near(r->endpoints, 2.0));
    QVERIFY(!r->linear_se);
  }

  void aRunTakesItsDetectorOnly() {
    RiseRun run;
    QVERIFY(!run.running());
    run.add(row(0, {{"H1", 9.0}}));  // not running: ignored
    run.start("H1");
    QVERIFY(run.running());
    QVERIFY(!run.current());
    run.add(row(0, {{"H1", 1.0}, {"AX", 100.0}}));
    run.add(row(30, {{"AX", 200.0}}));  // nothing of H1's
    run.add(row(60, {{"H1", 3.0}, {"AX", 300.0}}));
    const auto r = run.current();
    QVERIFY(r);
    QCOMPARE(r->n, 2);
    QVERIFY(near(r->endpoints, 2.0));
    QCOMPARE(*run.first(), at(0));
    QCOMPARE(*run.last(), at(60));
  }

  void aRunSkipsAReadingWithoutAValue() {
    RiseRun run;
    run.start("H1");
    run.add(row(0, {{"H1", 1.0}}));
    run.add(row(30, {{"H1", std::nullopt}}));
    run.add(row(60, {{"H1", 3.0}}));
    QCOMPARE(run.current()->n, 2);
    QVERIFY(!run.saturated());
    QVERIFY(!run.restarted());
  }

  void aRunMarksASaturatedReading() {
    RiseRun run;
    run.start("H1");
    run.add(row(0, {{"H1", 1.0}}));
    run.add(row(60, {{"H1", 3.0}}, true));
    QVERIFY(run.saturated());
    QCOMPARE(run.current()->n, 2);  // kept all the same
  }

  void timeGoingBackVoidsARun() {
    RiseRun run;
    run.start("H1");
    run.add(row(100, {{"H1", 1.0}}));
    run.add(row(160, {{"H1", 3.0}}));
    QVERIFY(run.current());
    run.add(row(5, {{"H1", 1.0}}));  // the scan began again
    QVERIFY(run.restarted());
    QVERIFY(!run.current());
    run.add(row(65, {{"H1", 3.0}}));  // and nothing after it mends that
    QVERIFY(!run.current());
    QVERIFY(run.restarted());
    // The same time twice is not a step back in time, but it is no new reading either.
    RiseRun same;
    same.start("H1");
    same.add(row(1, {{"H1", 1.0}}));
    same.add(row(1, {{"H1", 2.0}}));
    QVERIFY(same.restarted());
  }

  void currentFollowsEachReading() {
    RiseRun run;
    run.start("H1");
    run.add(row(0, {{"H1", 0.0}}));
    run.add(row(60, {{"H1", 1.0}}));
    QVERIFY(near(run.current()->linear, 1.0));
    run.add(row(120, {{"H1", 4.0}}));
    QVERIFY(near(run.current()->endpoints, 2.0));
    QCOMPARE(run.current()->n, 3);
  }

  void startAgainForgetsTheLast() {
    RiseRun run;
    run.start("H1");
    run.add(row(0, {{"H1", 0.0}}));
    run.add(row(60, {{"H1", 1.0}}, true));
    run.stop();
    QVERIFY(!run.running());
    QVERIFY(run.current());  // what it measured stays until the next start
    run.add(row(120, {{"H1", 9.0}}));  // stopped: ignored
    QCOMPARE(run.current()->n, 2);
    run.start("AX");
    QVERIFY(!run.current());
    QVERIFY(!run.saturated());
    QVERIFY(!run.first());
    QCOMPARE(run.detector(), std::string("AX"));
  }
};

QTEST_APPLESS_MAIN(TestRiseRate)
#include "test_rise_rate.moc"
