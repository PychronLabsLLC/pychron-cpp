// The rise rate calculation and a run of readings (pure data), and the panel
// that shows them.

#include <chrono>
#include <cmath>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QComboBox>
#include <QFile>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "rise_rate.hpp"
#include "rise_rate_panel.hpp"

using namespace pychron;
using pychron::spectrometer::IntensityReading;
using pychron::spectrometer::Value;
using pychron::ui::rise_rate;
using pychron::ui::RisePoint;
using pychron::ui::DetectorSeries;
using pychron::ui::RiseRatePanel;
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

bool close_to(double a, double b, double tolerance = 1e-9) { return std::abs(a - b) <= tolerance; }

std::vector<DetectorSeries> two() {
  DetectorSeries a;
  a.name = "H1";
  a.units = QStringLiteral("fA");
  a.isotope = QStringLiteral("Ar40");
  DetectorSeries b;
  b.name = "AX";
  b.units = QStringLiteral("cps");
  return {a, b};
}

QString cell(const RiseRatePanel& panel, int row, int column) { return panel.results()->item(row, column)->text(); }

// The noisy line of aNoisyLineHasAStandardError on H1.
void feed_noisy(RiseRatePanel& panel) {
  panel.on_reading(row(100, {{"H1", 0.0}, {"AX", 5.0}}));
  panel.on_reading(row(160, {{"H1", 1.1}, {"AX", 5.0}}));
  panel.on_reading(row(220, {{"H1", 1.9}, {"AX", 5.0}}));
  panel.on_reading(row(280, {{"H1", 3.1}, {"AX", 5.0}}));
}

}  // namespace

class TestRiseRate : public QObject {
  Q_OBJECT

 private slots:
  void endpointsAndSlopeByHand() {
    const std::vector<RisePoint> points{{0, 1}, {60, 3}, {120, 5}};
    const auto r = rise_rate(points);
    QVERIFY(r);
    QCOMPARE(r->n, 3);
    QVERIFY(close_to(r->duration_min, 2.0));
    QVERIFY(close_to(r->rise, 4.0));
    QVERIFY(close_to(r->endpoints, 2.0));
    QVERIFY(close_to(r->linear, 2.0));
    QVERIFY(r->linear_se);
    QVERIFY(close_to(*r->linear_se, 0.0));
  }

  // x 0, 1, 2, 3 min; y 0, 1.1, 1.9, 3.1: Sxx 5, Sxy 5.05, slope 1.01,
  // residuals -0.01, 0.08, -0.13, 0.06, SSR 0.027, se sqrt(0.027 / 2 / 5).
  void aNoisyLineHasAStandardError() {
    const std::vector<RisePoint> points{{0, 0}, {60, 1.1}, {120, 1.9}, {180, 3.1}};
    const auto r = rise_rate(points);
    QVERIFY(r);
    QVERIFY(close_to(r->linear, 1.01));
    QVERIFY(r->linear_se);
    QVERIFY(close_to(*r->linear_se, std::sqrt(0.0027)));
    QVERIFY(close_to(r->endpoints, 3.1 / 3.0));
  }

  // A reading that comes late. A fit that took the samples as evenly spaced
  // (legacy's) gives 1.82 here, not 2.
  void unevenSpacingGivesTheTrueSlope() {
    std::vector<RisePoint> points;
    for (const double t : {0.0, 10.0, 20.0, 300.0}) points.push_back({t, 2.0 * t / 60.0});
    const auto r = rise_rate(points);
    QVERIFY(r);
    QVERIFY(close_to(r->linear, 2.0));
    QVERIFY(close_to(r->endpoints, 2.0));
  }

  void nonFiniteValuesAreLeftOut() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::vector<RisePoint> points{{0, nan}, {60, 3}, {90, nan}, {120, 5}, {180, 7}, {240, nan}};
    const auto r = rise_rate(points);
    QVERIFY(r);
    QCOMPARE(r->n, 3);
    QVERIFY(close_to(r->duration_min, 2.0));
    QVERIFY(close_to(r->rise, 4.0));
    QVERIFY(close_to(r->linear, 2.0));
  }

  void onePointIsNoRate() {
    QVERIFY(!rise_rate(std::vector<RisePoint>{}));
    QVERIFY(!rise_rate(std::vector<RisePoint>{{5, 1}}));
    const double nan = std::numeric_limits<double>::quiet_NaN();
    QVERIFY(!rise_rate(std::vector<RisePoint>{{5, 1}, {6, nan}}));
  }

  // Hours into a session: the times are large, their differences are not.
  void lateTimesAreNoTrouble() {
    const double t0 = 1.0e9;
    const auto r = rise_rate(std::vector<RisePoint>{{t0, 1}, {t0 + 60, 3}, {t0 + 120, 5}});
    QVERIFY(r);
    QVERIFY(close_to(r->linear, 2.0));
    QVERIFY(close_to(*r->linear_se, 0.0, 1e-6));
  }

  void zeroDurationIsNoRate() { QVERIFY(!rise_rate(std::vector<RisePoint>{{5, 1}, {5, 2}})); }

  void twoPointsHaveNoStandardError() {
    const auto r = rise_rate(std::vector<RisePoint>{{0, 1}, {30, 2}});
    QVERIFY(r);
    QCOMPARE(r->n, 2);
    QVERIFY(close_to(r->linear, 2.0));
    QVERIFY(close_to(r->endpoints, 2.0));
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
    QVERIFY(close_to(r->endpoints, 2.0));
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
    // Nor does a value that is no number count, first, last or between: the
    // run's times are those of the readings its rate is made of.
    RiseRun nan;
    nan.start("H1");
    nan.add(row(0, {{"H1", std::numeric_limits<double>::quiet_NaN()}}));
    QVERIFY(!nan.first());
    nan.add(row(10, {{"H1", 1.0}}));
    nan.add(row(70, {{"H1", 3.0}}));
    nan.add(row(90, {{"H1", std::numeric_limits<double>::quiet_NaN()}}));
    QCOMPARE(*nan.first(), at(10));
    QCOMPARE(*nan.last(), at(70));
    QCOMPARE(nan.current()->n, 2);
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
    QVERIFY(close_to(run.current()->linear, 1.0));
    run.add(row(120, {{"H1", 4.0}}));
    QVERIFY(close_to(run.current()->endpoints, 2.0));
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

  void aRunMakesARowOfItsNumbers() {
    RiseRatePanel panel(two());
    QSignalSpy started(&panel, &RiseRatePanel::started);
    QSignalSpy stopped(&panel, &RiseRatePanel::stopped);
    QSignalSpy cleared(&panel, &RiseRatePanel::cleared);
    QCOMPARE(panel.toggle_button()->text(), QStringLiteral("Start"));
    panel.on_reading(row(50, {{"H1", 9.0}}));  // before Start: nothing
    QVERIFY(!panel.running());

    panel.toggle();
    QVERIFY(panel.running());
    QCOMPARE(panel.toggle_button()->text(), QStringLiteral("Stop"));
    QVERIFY(!panel.detector_box()->isEnabled());
    QCOMPARE(cleared.count(), 1);  // the last run's marks go
    QCOMPARE(started.count(), 0);  // no reading yet
    feed_noisy(panel);
    QCOMPARE(started.count(), 1);
    QCOMPARE(started.at(0).at(0).value<TimePoint>(), at(100));
    panel.toggle();
    QVERIFY(!panel.running());
    QVERIFY(panel.detector_box()->isEnabled());
    QCOMPARE(stopped.count(), 1);
    QCOMPARE(stopped.at(0).at(0).value<TimePoint>(), at(280));
    QCOMPARE(cleared.count(), 1);

    QCOMPARE(panel.results()->rowCount(), 1);
    QCOMPARE(cell(panel, 0, RiseRatePanel::ColNumber), QStringLiteral("1"));
    QCOMPARE(cell(panel, 0, RiseRatePanel::ColDetector), QStringLiteral("H1 (Ar40)"));
    QCOMPARE(cell(panel, 0, RiseRatePanel::ColEndpoints), QStringLiteral("1.033"));
    QCOMPARE(cell(panel, 0, RiseRatePanel::ColLinear), QStringLiteral("1.01"));
    QCOMPARE(cell(panel, 0, RiseRatePanel::ColError), QStringLiteral("0.052"));
    QCOMPARE(cell(panel, 0, RiseRatePanel::ColUnits), QStringLiteral("fA/min"));
    QCOMPARE(cell(panel, 0, RiseRatePanel::ColMinutes), QStringLiteral("3"));
    QCOMPARE(cell(panel, 0, RiseRatePanel::ColReadings), QStringLiteral("4"));
    QCOMPARE(cell(panel, 0, RiseRatePanel::ColTime), QString());  // no clock given
    QCOMPARE(cell(panel, 0, RiseRatePanel::ColNote), QString());
    QVERIFY(panel.status_text().isEmpty());
  }

  void theRateShowsWhileItRuns() {
    RiseRatePanel panel(two());
    panel.toggle();
    QVERIFY(panel.live_text().contains(QStringLiteral("Waiting")));
    panel.on_reading(row(100, {{"H1", 0.0}}));
    QVERIFY(panel.live_text().contains(QStringLiteral("1 reading")));
    panel.on_reading(row(160, {{"H1", 1.0}}));
    const QString two_readings = panel.live_text();
    QVERIFY2(two_readings.contains(QStringLiteral("2 readings")) && two_readings.contains(QStringLiteral("fA/min")),
             qPrintable(two_readings));
    panel.on_reading(row(220, {{"H1", 4.0}}));
    QVERIFY(panel.live_text() != two_readings);
    QVERIFY(panel.live_text().contains(QStringLiteral("±")));
    panel.toggle();
    QVERIFY(panel.live_text().isEmpty());
  }

  void tooFewReadingsMakeNoRow() {
    RiseRatePanel panel(two());
    QSignalSpy stopped(&panel, &RiseRatePanel::stopped);
    QSignalSpy cleared(&panel, &RiseRatePanel::cleared);
    panel.toggle();
    panel.on_reading(row(100, {{"H1", 0.0}}));
    panel.toggle();
    QCOMPARE(panel.results()->rowCount(), 0);
    QVERIFY2(panel.status_text().contains(QStringLiteral("fewer than two")), qPrintable(panel.status_text()));
    QCOMPARE(stopped.count(), 0);
    QCOMPARE(cleared.count(), 2);  // at Start, and now: the start mark goes
    panel.toggle();                // the next Start clears what was said
    QVERIFY(panel.status_text().isEmpty());
  }

  void aRunTheScanRestartedUnderMakesNoRow() {
    RiseRatePanel panel(two());
    QSignalSpy stopped(&panel, &RiseRatePanel::stopped);
    QSignalSpy cleared(&panel, &RiseRatePanel::cleared);
    panel.toggle();
    panel.on_reading(row(100, {{"H1", 0.0}}));
    panel.on_reading(row(160, {{"H1", 1.0}}));
    QCOMPARE(cleared.count(), 1);
    panel.on_reading(row(3, {{"H1", 0.0}}));
    QVERIFY(panel.live_text().contains(QStringLiteral("began again")));
    QCOMPARE(cleared.count(), 2);  // its start mark goes at once
    panel.on_reading(row(63, {{"H1", 1.0}}));
    panel.toggle();
    QCOMPARE(cleared.count(), 2);  // and is not taken away twice
    QCOMPARE(panel.results()->rowCount(), 0);
    QVERIFY(panel.status_text().contains(QStringLiteral("began again")));
    QCOMPARE(stopped.count(), 0);
  }

  void copyAndClear() {
    RiseRatePanel panel(two());
    QVERIFY(!panel.clear_button()->isEnabled());
    for (int i = 0; i < 2; ++i) {
      panel.toggle();
      feed_noisy(panel);
      panel.toggle();
    }
    QCOMPARE(panel.results()->rowCount(), 2);
    QCOMPARE(cell(panel, 1, RiseRatePanel::ColNumber), QStringLiteral("2"));
    const QStringList lines = panel.copy_text().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(lines.size(), 3);
    QCOMPARE(lines[0].split(QLatin1Char('\t')).size(), static_cast<qsizetype>(RiseRatePanel::ColCount));
    QVERIFY(lines[0].startsWith(QStringLiteral("#\tDetector\tEndpoints\tLinear")));
    QVERIFY(lines[2].startsWith(QStringLiteral("2\tH1 (Ar40)\t1.033\t1.01\t0.052\tfA/min\t3\t4")));

    QVERIFY(panel.clear_button()->isEnabled());
    panel.clear_button()->click();
    QCOMPARE(panel.results()->rowCount(), 0);
    panel.toggle();
    feed_noisy(panel);
    panel.toggle();
    QCOMPARE(cell(panel, 0, RiseRatePanel::ColNumber), QStringLiteral("1"));  // numbered from 1 again
  }

  void theDetectorIsChosenBeforeARun() {
    RiseRatePanel panel(two());
    panel.set_detector(QStringLiteral("AX"));
    QCOMPARE(panel.detector_box()->currentText(), QStringLiteral("AX"));
    panel.set_detector(QStringLiteral("ZZ"));  // unknown: ignored
    QCOMPARE(panel.detector_box()->currentText(), QStringLiteral("AX"));
    panel.toggle();
    panel.set_detector(QStringLiteral("H1"));  // running: ignored
    QCOMPARE(panel.detector_box()->currentText(), QStringLiteral("AX"));
    panel.on_reading(row(0, {{"H1", 0.0}, {"AX", 10.0}}));
    panel.on_reading(row(30, {{"H1", 50.0}, {"AX", 11.0}}));
    panel.toggle();
    QCOMPARE(cell(panel, 0, RiseRatePanel::ColDetector), QStringLiteral("AX"));  // no isotope known for it
    QCOMPARE(cell(panel, 0, RiseRatePanel::ColEndpoints), QStringLiteral("2"));
    QCOMPARE(cell(panel, 0, RiseRatePanel::ColUnits), QStringLiteral("cps/min"));
    QCOMPARE(cell(panel, 0, RiseRatePanel::ColError), QString());  // two readings: no error

    panel.set_isotope("AX", QStringLiteral("Ar39"));
    panel.toggle();
    panel.on_reading(row(100, {{"AX", 10.0}}));
    panel.on_reading(row(130, {{"AX", 11.0}}));
    panel.toggle();
    QCOMPARE(cell(panel, 1, RiseRatePanel::ColDetector), QStringLiteral("AX (Ar39)"));
  }

  void aResultTakesTheTimeItIsGiven() {
    RiseRatePanel panel(two());
    panel.set_wall_clock([] { return QDateTime(QDate(2026, 10, 13), QTime(8, 30, 5)); });
    panel.toggle();
    feed_noisy(panel);
    panel.toggle();
    QCOMPARE(cell(panel, 0, RiseRatePanel::ColTime), QStringLiteral("2026-10-13 08:30:05"));
  }

  void anAbandonedRunGivesNoResult() {
    RiseRatePanel panel(two());
    QSignalSpy stopped(&panel, &RiseRatePanel::stopped);
    QSignalSpy cleared(&panel, &RiseRatePanel::cleared);
    panel.abandon(QStringLiteral("nothing was running"));  // no run: nothing
    QVERIFY(panel.status_text().isEmpty());
    QCOMPARE(cleared.count(), 0);
    panel.toggle();
    feed_noisy(panel);
    panel.abandon(QStringLiteral("the window was closed while it ran"));
    QVERIFY(!panel.running());
    QCOMPARE(panel.results()->rowCount(), 0);
    QCOMPARE(stopped.count(), 0);
    QCOMPARE(cleared.count(), 2);
    QCOMPARE(panel.toggle_button()->text(), QStringLiteral("Start"));
    QVERIFY(panel.detector_box()->isEnabled());
    QVERIFY(panel.live_text().isEmpty());
    QVERIFY2(panel.status_text().contains(QStringLiteral("window was closed")), qPrintable(panel.status_text()));
    panel.on_reading(row(400, {{"H1", 9.0}}));  // and what comes after is no part of it
    QVERIFY(panel.live_text().isEmpty());
  }

  void aPanelWithNoDetectorsDoesNothing() {
    RiseRatePanel panel({});
    QVERIFY(!panel.toggle_button()->isEnabled());
    panel.toggle();
    QVERIFY(!panel.running());
  }

  void eachResultIsAppendedToTheFile() {
    QTemporaryDir dir;
    const std::filesystem::path path = std::filesystem::path(dir.path().toStdString()) / "sub" / "rise_rates.csv";
    const auto lines = [&] {
      QFile file(QString::fromStdString(path.string()));
      if (!file.open(QIODevice::ReadOnly)) return QStringList{};
      return QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    };
    {
      RiseRatePanel panel(two());
      panel.set_wall_clock([] { return QDateTime(QDate(2026, 10, 13), QTime(8, 30, 5)); });
      panel.set_results_file(path, QStringLiteral("argus, the old one"), true);
      QVERIFY(lines().isEmpty());  // nothing until there is a result
      panel.toggle();
      feed_noisy(panel);
      panel.toggle();
      QVERIFY2(panel.status_text().isEmpty(), qPrintable(panel.status_text()));
      const QStringList first = lines();
      QCOMPARE(first.size(), 2);  // made with its folder and its header
      QCOMPARE(first[0], QStringLiteral("time,spectrometer,simulated,detector,isotope,endpoints,linear,linear_se,units,"
                                        "minutes,n,note"));
      // Numbers whole, not rounded as the table has them; a name with a comma quoted.
      QVERIFY2(first[1].startsWith(QStringLiteral("2026-10-13 08:30:05,\"argus, the old one\",yes,H1,Ar40,1.033333333,1.01,")),
               qPrintable(first[1]));
      QVERIFY(first[1].endsWith(QStringLiteral(",fA/min,3,4,")));
      panel.clear_button()->click();  // the table's, not the file's
      panel.toggle();
      panel.on_reading(row(0, {{"H1", 1.0}}));
      panel.on_reading(row(60, {{"H1", 3.0}}, true));
      panel.toggle();
      QCOMPARE(lines().size(), 3);
      QVERIFY(lines()[2].endsWith(QStringLiteral(",fA/min,1,2,saturated")));
      QVERIFY(lines()[2].contains(QStringLiteral(",2,2,,fA/min,")));  // two readings: no standard error
    }
    // Another session adds to the same file, under the one header.
    RiseRatePanel later(two());
    later.set_results_file(path, QStringLiteral("argus"), false);
    later.set_detector(QStringLiteral("AX"));
    later.toggle();
    later.on_reading(row(0, {{"AX", 10.0}}));
    later.on_reading(row(30, {{"AX", 11.0}}));
    later.toggle();
    const QStringList all = lines();
    QCOMPARE(all.size(), 4);
    QVERIFY2(all[3].startsWith(QStringLiteral(",argus,,AX,,2,2,,cps/min,0.5,2,")), qPrintable(all[3]));
    QCOMPARE(all.filter(QStringLiteral("time,spectrometer")).size(), 1);
  }

  void aFileThatCannotBeWrittenIsSaidAndTheResultKept() {
    QTemporaryDir dir;
    // A folder where the file should be: it cannot be opened for writing.
    const std::filesystem::path path = std::filesystem::path(dir.path().toStdString()) / "rise_rates.csv";
    std::filesystem::create_directories(path);
    RiseRatePanel panel(two());
    panel.set_results_file(path, QStringLiteral("argus"), false);
    panel.toggle();
    feed_noisy(panel);
    panel.toggle();
    QCOMPARE(panel.results()->rowCount(), 1);
    QVERIFY2(panel.status_text().contains(QStringLiteral("could not be written")), qPrintable(panel.status_text()));
  }

  void aSaturatedRunSaysSo() {
    RiseRatePanel panel(two());
    panel.toggle();
    panel.on_reading(row(0, {{"H1", 1.0}}));
    panel.on_reading(row(60, {{"H1", 3.0}}, true));
    panel.toggle();
    QCOMPARE(cell(panel, 0, RiseRatePanel::ColNote), QStringLiteral("saturated"));
  }
};

QTEST_MAIN(TestRiseRate)
#include "test_rise_rate.moc"
