// The peak center graph (peak center plot design 4.4), fed by hand: the
// tries, their points, the outcome and the kept results.

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <QSignalSpy>
#include <QtTest/QtTest>

#include <qcustomplot.h>

#include "peak_center_view.hpp"
#include "plot_style.hpp"

using pychron::jobs::SweepPoint;
using pychron::ui::PeakCenterView;
namespace jobs = pychron::jobs;

namespace {

SweepPoint sweep_point(double x, std::map<std::string, double> y) {
  SweepPoint p;
  p.x = x;
  p.y = std::move(y);
  return p;
}

jobs::PeakCenterTryStarted try_started(int index, int n, std::vector<std::string> additional = {"AX"}) {
  jobs::PeakCenterTryStarted e;
  e.detector = "H1";
  e.isotope = "Ar40";
  e.additional = std::move(additional);
  e.try_index = index;
  e.n_tries = n;
  e.start = 4.9;
  e.end = 5.1;
  e.initial_center = 5.0;
  return e;
}

jobs::PeakCenterPoint point(int try_index, SweepPoint p) { return jobs::PeakCenterPoint{try_index, std::move(p)}; }

jobs::PeakShape shape() {
  jobs::PeakShape s;
  s.low_x = 5.11;
  s.center_x = 5.12;
  s.high_x = 5.13;
  s.low_y = 20;
  s.center_y = 100;
  s.high_y = 21;
  s.max_x = 5.121;
  s.max_y = 101;
  return s;
}

// Two tries: the first H1 {0, 50, 0} and AX {1, 2, 1}, the second H1 {0, 100,
// 0} and AX {1, 3, 1}.
std::vector<jobs::PeakCenterTry> two_tries() {
  jobs::PeakCenterTry first;
  first.start = 4.9;
  first.end = 5.1;
  first.points = {sweep_point(5.0, {{"H1", 0}, {"AX", 1}}), sweep_point(5.1, {{"H1", 50}, {"AX", 2}}),
                  sweep_point(5.2, {{"H1", 0}, {"AX", 1}})};
  jobs::PeakCenterTry second = first;
  second.points = {sweep_point(5.0, {{"H1", 0}, {"AX", 1}}), sweep_point(5.1, {{"H1", 100}, {"AX", 3}}),
                   sweep_point(5.2, {{"H1", 0}, {"AX", 1}})};
  return {first, second};
}

jobs::PeakCenterResult result(bool ok = true) {
  jobs::PeakCenterResult r;
  r.config.additional_detectors = {"AX"};
  r.config.n_tries = 2;
  r.ok = ok;
  r.message = ok ? "" : "no peak";
  r.detector = "H1";
  r.isotope = "Ar40";
  r.initial_center = 5.0;
  if (ok) {
    r.center = 5.123456;
    r.table_updated = true;
    r.shape = shape();
  }
  r.tries = two_tries();
  return r;
}

void feed_live(PeakCenterView& view, const std::vector<jobs::PeakCenterTry>& tries) {
  for (std::size_t i = 0; i < tries.size(); ++i) {
    view.on_try_started(try_started(static_cast<int>(i), static_cast<int>(tries.size())));
    for (const auto& p : tries[i].points) view.on_point(point(static_cast<int>(i), p));
  }
}

QVector<double> values(std::initializer_list<double> v) { return QVector<double>(v); }

}  // namespace

class TestPeakCenterView : public QObject {
  Q_OBJECT

 private slots:
  void cleanup() { pychron::ui::set_plot_style(pychron::ui::default_plot_style()); }

  void background_and_pens_follow_a_palette_change() {
    PeakCenterView view({{"AX", QColor("#654321")}});
    feed_live(view, two_tries());
    QCOMPARE(view.plot()->axisRect()->backgroundBrush().color(), pychron::ui::plot_style().plot_background);
    pychron::ui::PlotStyle style = pychron::ui::default_plot_style();
    style.plot_background = QColor("#fafad2");
    style.series[0] = QColor("#123456");
    pychron::ui::set_plot_style(style);
    QCOMPARE(view.plot()->axisRect()->backgroundBrush().color().name(), QStringLiteral("#fafad2"));
    QCOMPARE(view.plot()->graph(0)->pen().color().name(), QStringLiteral("#123456"));  // H1: the palette's
    QCOMPARE(view.plot()->graph(1)->pen().color().name(), QStringLiteral("#654321"));  // AX: its own
    QCOMPARE(view.series().size(), std::size_t{4});  // both tries still drawn
  }

  void a_palette_slot_gives_an_unconfigured_detector_its_colour() {
    PeakCenterView view({}, {{"H1", 3}});
    feed_live(view, two_tries());
    QCOMPARE(view.plot()->graph(0)->pen().color(), pychron::ui::plot_series_color(3));
  }

  void pointsFedOneByOneGrowTheReferenceSeries() {
    PeakCenterView view;
    view.on_try_started(try_started(0, 2, {}));
    view.on_point(point(0, sweep_point(5.0, {{"H1", 1}})));
    view.on_point(point(0, sweep_point(5.1, {{"H1", 4}})));
    view.on_point(point(0, sweep_point(5.2, {{"H1", 2}})));
    const auto s = view.series();
    QCOMPARE(s.size(), std::size_t{1});
    QCOMPARE(s[0].name, QStringLiteral("*H1"));
    QCOMPARE(s[0].x, values({5.0, 5.1, 5.2}));
    QCOMPARE(s[0].y, values({1, 4, 2}));
    QCOMPARE(s[0].raw, s[0].y);
    QVERIFY(!s[0].faint);
  }

  void anAdditionalDetectorIsScaledOntoTheReference() {
    PeakCenterView view({{"AX", QColor(Qt::red)}});
    view.on_try_started(try_started(0, 2));
    view.on_point(point(0, sweep_point(5.0, {{"H1", 0}, {"AX", 1}})));
    view.on_point(point(0, sweep_point(5.1, {{"H1", 100}, {"AX", 3}})));
    view.on_point(point(0, sweep_point(5.2, {{"H1", 0}, {"AX", 1}})));
    const auto s = view.series();
    QCOMPARE(s.size(), std::size_t{2});
    QCOMPARE(s[1].name, QStringLiteral("AX"));
    QCOMPARE(s[1].y, values({0, 100, 0}));
    QCOMPARE(s[1].raw, values({1, 3, 1}));
    QCOMPARE(view.plot()->graph(1)->pen().color(), QColor(Qt::red));
  }

  void aFlatAdditionalDetectorIsDrawnRaw() {
    PeakCenterView view;
    view.on_try_started(try_started(0, 2));
    view.on_point(point(0, sweep_point(5.0, {{"H1", 0}, {"AX", 2}})));
    view.on_point(point(0, sweep_point(5.1, {{"H1", 100}, {"AX", 2}})));
    const auto s = view.series();
    QCOMPARE(s[1].y, values({2, 2}));
    QCOMPARE(s[1].raw, values({2, 2}));
  }

  void aStepWithoutADetectorsValueIsLeftOut() {
    PeakCenterView view;
    view.on_try_started(try_started(0, 2));
    view.on_point(point(0, sweep_point(5.0, {{"H1", 0}, {"AX", 1}})));
    view.on_point(point(0, sweep_point(5.1, {{"H1", 100}})));
    view.on_point(point(0, sweep_point(5.2, {{"H1", 0}, {"AX", 3}})));
    const auto s = view.series();
    QCOMPARE(s[0].x.size(), 3);
    QCOMPARE(s[1].x, values({5.0, 5.2}));
    QCOMPARE(s[1].raw, values({1, 3}));
  }

  void scalingIsRecomputedAtEachPoint() {
    PeakCenterView view;
    view.on_try_started(try_started(0, 2));
    view.on_point(point(0, sweep_point(5.0, {{"H1", 0}, {"AX", 1}})));
    view.on_point(point(0, sweep_point(5.1, {{"H1", 10}, {"AX", 2}})));
    QCOMPARE(view.series()[1].y, values({0, 10}));
    view.on_point(point(0, sweep_point(5.2, {{"H1", 100}, {"AX", 2}})));
    QCOMPARE(view.series()[1].y, values({0, 100, 100}));
  }

  void theTitleNamesIsotopeDetectorAndTry() {
    PeakCenterView view;
    view.on_try_started(try_started(0, 2));
    QCOMPARE(view.title(), QStringLiteral("Ar40 on H1, try 1 of 2"));
    view.on_try_started(try_started(1, 2));
    QCOMPARE(view.title(), QStringLiteral("Ar40 on H1, try 2 of 2"));
  }

  void doneOkDrawsMarkersAndRules() {
    PeakCenterView view;
    view.on_done(jobs::PeakCenterDone{result()});
    QCOMPARE(view.marker_x(), values({5.11, 5.12, 5.13, 5.121}));
    QCOMPARE(view.rule_x(), values({5.0, 5.123456}));
    QVERIFY2(view.title().endsWith(QStringLiteral("center 5.123456, table updated")), qPrintable(view.title()));
  }

  void aDacOffsetAddsAThirdRule() {
    PeakCenterView view;
    auto r = result();
    r.config.dac_offset = 0.01;
    view.on_done(jobs::PeakCenterDone{r});
    const auto rules = view.rule_x();
    QCOMPARE(rules.size(), 3);
    QVERIFY(qFuzzyCompare(rules[2], 5.123456 - 0.01));
  }

  void doneFailedKeepsThePointsAndDrawsNoMarkers() {
    PeakCenterView view;
    view.on_done(jobs::PeakCenterDone{result(false)});
    QCOMPARE(view.title(), QStringLiteral("Ar40 on H1: failed: no peak"));
    QVERIFY(view.marker_x().isEmpty());
    QVERIFY(!view.series().empty());
    QCOMPARE(view.series()[0].x.size(), 3);
  }

  void aSecondTryLeavesTheFirstFaint() {
    PeakCenterView view;
    view.on_try_started(try_started(0, 2));
    view.on_point(point(0, sweep_point(5.0, {{"H1", 7}, {"AX", 1}})));
    view.on_point(point(0, sweep_point(5.1, {{"H1", 9}, {"AX", 2}})));
    view.on_try_started(try_started(1, 2));
    view.on_point(point(1, sweep_point(5.0, {{"H1", 3}, {"AX", 1}})));
    const auto s = view.series();
    QCOMPARE(s.size(), std::size_t{4});
    QCOMPARE(s[0].name, QStringLiteral("*H1"));
    QCOMPARE(s[0].y, values({3}));
    QVERIFY(!s[0].faint && !s[1].faint);
    QVERIFY(s[2].faint && s[3].faint);
    QVERIFY(s[2].name.isEmpty() && s[3].name.isEmpty());
    QCOMPARE(s[2].y, values({7, 9}));
  }

  void scalingIsWithinATry() {
    PeakCenterView view;
    view.on_try_started(try_started(0, 2));
    view.on_point(point(0, sweep_point(5.0, {{"H1", 0}, {"AX", 1}})));
    view.on_point(point(0, sweep_point(5.1, {{"H1", 10}, {"AX", 2}})));
    view.on_try_started(try_started(1, 2));
    view.on_point(point(1, sweep_point(5.0, {{"H1", 0}, {"AX", 1}})));
    view.on_point(point(1, sweep_point(5.1, {{"H1", 1000}, {"AX", 2}})));
    const auto s = view.series();
    QCOMPARE(s[1].y, values({0, 1000}));
    QCOMPARE(s[3].y, values({0, 10}));
  }

  void doneAloneDrawsEveryTry() {
    PeakCenterView live;
    feed_live(live, two_tries());
    PeakCenterView done;
    done.on_done(jobs::PeakCenterDone{result()});
    const auto a = live.series();
    const auto b = done.series();
    QCOMPARE(a.size(), b.size());
    QCOMPARE(b.size(), std::size_t{4});
    for (std::size_t i = 0; i < a.size(); ++i) {
      QCOMPARE(a[i].name, b[i].name);
      QCOMPARE(a[i].x, b[i].x);
      QCOMPARE(a[i].y, b[i].y);
      QCOMPARE(a[i].raw, b[i].raw);
      QCOMPARE(a[i].faint, b[i].faint);
    }
    QCOMPARE(done.rule_x().value(0), live.rule_x().value(0));
  }

  void aResultWithoutATryIsStillRuled() {
    PeakCenterView view;
    auto r = result();
    r.tries.clear();
    view.on_done(jobs::PeakCenterDone{r});
    QVERIFY(view.series().empty());
    QCOMPARE(view.rule_x(), values({5.0, 5.123456}));
  }

  void doneReplacesWhatWasDrawnLive() {
    PeakCenterView view;
    view.on_try_started(try_started(0, 2));
    view.on_point(point(0, sweep_point(9.0, {{"H1", 77}})));
    view.on_done(jobs::PeakCenterDone{result()});
    for (const auto& s : view.series()) QVERIFY(!s.x.contains(9.0));
  }

  void theNewestTwentyResultsAreKept() {
    PeakCenterView view;
    for (int i = 0; i < 22; ++i) {
      auto r = result();
      r.center = 5.0 + i;
      view.on_done(jobs::PeakCenterDone{r});
    }
    QCOMPARE(view.kept(), 20);
    view.show_result(0);
    QCOMPARE(view.rule_x().value(1), 7.0);
  }

  void showResultRestoresAnEarlierOne() {
    PeakCenterView view;
    view.on_done(jobs::PeakCenterDone{result()});
    view.on_done(jobs::PeakCenterDone{result(false)});
    QVERIFY(view.title().contains(QStringLiteral("failed")));
    view.show_result(0);
    QVERIFY(view.title().endsWith(QStringLiteral("table updated")));
    QCOMPARE(view.marker_x().size(), 4);
    QCOMPARE(view.kept(), 2);
  }

  void aNewTryAfterShowingAnOldResultShowsTheNewOne() {
    PeakCenterView view;
    view.on_done(jobs::PeakCenterDone{result()});
    view.on_done(jobs::PeakCenterDone{result()});
    view.show_result(0);
    view.on_try_started(try_started(0, 2));
    view.on_point(point(0, sweep_point(4.0, {{"H1", 5}})));
    QCOMPARE(view.title(), QStringLiteral("Ar40 on H1, try 1 of 2"));
    QCOMPARE(view.series().size(), std::size_t{2});
    QCOMPARE(view.series()[0].x, values({4.0}));
    QVERIFY(view.marker_x().isEmpty());
  }

  void clearEmptiesTheGraphAndTheKeptResults() {
    PeakCenterView view;
    view.on_done(jobs::PeakCenterDone{result()});
    view.clear();
    QCOMPARE(view.kept(), 0);
    QVERIFY(view.series().empty());
    QVERIFY(view.marker_x().isEmpty());
    QVERIFY(view.rule_x().isEmpty());
    QCOMPARE(view.plot()->graphCount(), 0);
  }

  void tryStartedIsEmittedOnce() {
    PeakCenterView view;
    QSignalSpy spy(&view, &PeakCenterView::tryStarted);
    view.on_try_started(try_started(0, 2));
    QCOMPARE(spy.count(), 1);
    view.on_point(point(0, sweep_point(5.0, {{"H1", 1}})));
    view.on_try_started(try_started(1, 2));
    QCOMPARE(spy.count(), 2);
  }

  void theTooltipGivesTheRawIntensity() {
    PeakCenterView view;
    view.on_try_started(try_started(0, 2));
    view.on_point(point(0, sweep_point(5.0, {{"H1", 0}, {"AX", 0.25}})));
    view.on_point(point(0, sweep_point(5.1, {{"H1", 777}, {"AX", 0.75}})));
    const QString tip = view.tooltip_at(1, 1);
    QVERIFY2(tip.contains(QStringLiteral("AX")), qPrintable(tip));
    QVERIFY2(tip.contains(QStringLiteral("0.75")), qPrintable(tip));
    QVERIFY2(!tip.contains(QStringLiteral("777")), qPrintable(tip));
    QVERIFY2(tip.contains(QStringLiteral("5.1")), qPrintable(tip));
  }

  void theInitialCentreIsRuledFromTheTrysStart() {
    PeakCenterView view;
    view.on_try_started(try_started(0, 2));
    QCOMPARE(view.rule_x(), values({5.0}));
  }

  void theReferenceIsHeavierAndInTheLegend() {
    PeakCenterView view;
    feed_live(view, two_tries());
    auto* plot = view.plot();
    QVERIFY(plot->graph(0)->pen().widthF() > plot->graph(1)->pen().widthF());
    QStringList legend;
    for (int i = 0; i < plot->legend->itemCount(); ++i)
      if (auto* item = qobject_cast<QCPPlottableLegendItem*>(plot->legend->item(i)))
        legend << item->plottable()->name();
    QCOMPARE(legend, QStringList({QStringLiteral("*H1"), QStringLiteral("AX")}));
  }

  void markersSitOnTheReferencesValues() {
    PeakCenterView view;
    view.on_done(jobs::PeakCenterDone{result()});
    QCOMPARE(view.marker_y(), values({20, 100, 21, 101}));
  }

  void aTryLeftWithoutAnOutcomeSaysSo() {
    PeakCenterView view;
    view.on_try_started(try_started(0, 2));
    view.on_point(point(0, sweep_point(5.0, {{"H1", 1}})));
    view.on_point(point(0, sweep_point(5.1, {{"H1", 2}})));
    view.abandon();
    QCOMPARE(view.title(), QStringLiteral("Ar40 on H1, try 1 of 2: not finished"));
    QCOMPARE(view.series()[0].x.size(), 2);
    QCOMPARE(view.kept(), 0);
  }

  // ---- the axes: x the sweep's whole width, y the data's -------------------

  void xSpansTheTryFromItsStart() {
    PeakCenterView view;
    view.on_try_started(try_started(0, 2));
    QCOMPARE(view.x_range().lower, 4.9);
    QCOMPARE(view.x_range().upper, 5.1);
    view.on_point(point(0, sweep_point(4.95, {{"H1", 1}})));
    view.on_point(point(0, sweep_point(4.96, {{"H1", 3}})));
    view.on_point(point(0, sweep_point(4.97, {{"H1", 2}})));
    QCOMPARE(view.x_range().lower, 4.9);
    QCOMPARE(view.x_range().upper, 5.1);
  }

  void aDescendingSweepSpansTheSameWidth() {
    PeakCenterView view;
    auto e = try_started(0, 1);
    std::swap(e.start, e.end);
    view.on_try_started(e);
    QCOMPARE(view.x_range().lower, 4.9);
    QCOMPARE(view.x_range().upper, 5.1);
  }

  void aWiderTryWidensTheAxisOnce() {
    PeakCenterView view;
    view.on_try_started(try_started(0, 2));
    view.on_point(point(0, sweep_point(5.0, {{"H1", 1}})));
    auto wider = try_started(1, 2);
    wider.start = 4.8;
    wider.end = 5.05;
    view.on_try_started(wider);
    QCOMPARE(view.x_range().lower, 4.8);
    QCOMPARE(view.x_range().upper, 5.1);  // the union, the first try's end kept
    view.on_point(point(1, sweep_point(4.85, {{"H1", 2}})));
    QCOMPARE(view.x_range().lower, 4.8);
    QCOMPARE(view.x_range().upper, 5.1);
  }

  void aNewPeakCenterForgetsTheOldWidth() {
    PeakCenterView view;
    auto wide = try_started(0, 1);
    wide.start = 4.0;
    wide.end = 6.0;
    view.on_try_started(wide);
    view.on_try_started(try_started(0, 1));  // try 0 again: a new peak center
    QCOMPARE(view.x_range().lower, 4.9);
    QCOMPARE(view.x_range().upper, 5.1);
  }

  void yHasASaneRangeBeforeAnyPoint() {
    PeakCenterView view;
    view.on_try_started(try_started(0, 2));
    QVERIFY(view.y_range().upper > view.y_range().lower);
  }

  void yCoversThePointsWithRoom() {
    PeakCenterView view;
    view.on_try_started(try_started(0, 1, {}));
    view.on_point(point(0, sweep_point(4.95, {{"H1", 10}})));
    view.on_point(point(0, sweep_point(4.96, {{"H1", 30}})));
    QVERIFY(view.y_range().lower < 10);
    QVERIFY(view.y_range().upper > 30);
    QVERIFY(view.y_range().lower > 8);  // some 5 % of the 20 span, not a jump to zero
    QVERIFY(view.y_range().upper < 32);
    view.on_point(point(0, sweep_point(4.97, {{"H1", 100}})));
    QVERIFY(view.y_range().upper > 100);
    QVERIFY(view.y_range().upper < 106);
  }

  void xIsZeroToOneWithNoSweep() {
    PeakCenterView view;
    QCOMPARE(view.x_range().lower, 0.0);
    QCOMPARE(view.x_range().upper, 1.0);
    view.on_try_started(try_started(0, 1));
    view.on_point(point(0, sweep_point(5.0, {{"H1", 1}})));
    view.clear();  // the old sweep's width does not stay
    QCOMPARE(view.x_range().lower, 0.0);
    QCOMPARE(view.x_range().upper, 1.0);
    QCOMPARE(view.y_range().lower, 0.0);
    QCOMPARE(view.y_range().upper, 1.0);
  }

  // No try, so no sweep: x is around the rules, which stay in sight.
  void aResultWithoutATryKeepsItsRulesInSight() {
    PeakCenterView view;
    auto r = result();
    r.tries.clear();
    view.on_done(jobs::PeakCenterDone{r});
    for (const double x : view.rule_x()) {
      QVERIFY(view.x_range().lower < x);
      QVERIFY(view.x_range().upper > x);
    }
  }

  void aShownResultSpansItsTries() {
    PeakCenterView view;
    auto r = result();
    r.tries[0].start = 4.95;
    r.tries[0].end = 5.25;
    r.tries[1].start = 4.85;
    r.tries[1].end = 5.15;
    view.on_done(jobs::PeakCenterDone{r});
    QCOMPARE(view.x_range().lower, 4.85);
    QCOMPARE(view.x_range().upper, 5.25);
    QVERIFY(view.y_range().upper > 101);  // the maximum marker, inside
    view.on_try_started(try_started(0, 1));  // a new one: its own width
    QCOMPARE(view.x_range().lower, 4.9);
    QCOMPARE(view.x_range().upper, 5.1);
    view.show_result(0);  // the kept one: its tries' again
    QCOMPARE(view.x_range().lower, 4.85);
    QCOMPARE(view.x_range().upper, 5.25);
  }
};

QTEST_MAIN(TestPeakCenterView)
#include "test_peak_center_view.moc"
