#include <chrono>
#include <cmath>
#include <limits>
#include <vector>

#include <QDateTime>
#include <QLabel>
#include <QStackedWidget>
#include <QtTest/QtTest>

#include <qcustomplot.h>

#include "history_chart.hpp"
#include "plot_style.hpp"
#include "reading_history.hpp"

using namespace std::chrono_literals;
using pychron::TimeSeriesRing;
using pychron::ui::HistoryChart;
using pychron::ui::ReadingHistory;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// t: seconds before the newest reading (<= 0).
ReadingHistory::Series series_of(std::vector<TimeSeriesRing::Point> points) {
  ReadingHistory::Series s;
  s.points = std::move(points);
  s.units = "torr";
  if (!s.points.empty()) s.newest = pychron::TimePoint{} + 1h;
  return s;
}

QDateTime half_past_eight() { return QDateTime(QDate(2026, 10, 13), QTime(8, 30, 0)); }

double key_of(const QDateTime& wall) { return static_cast<double>(wall.toMSecsSinceEpoch()) / 1000.0; }

QCPGraph* graph_of(HistoryChart& chart) {
  auto* plot = chart.findChild<QCustomPlot*>();
  return plot != nullptr && plot->graphCount() > 0 ? plot->graph(0) : nullptr;
}

}  // namespace

class TestHistoryChart : public QObject {
  Q_OBJECT

 private slots:
  void cleanup() { pychron::ui::set_plot_style(pychron::ui::default_plot_style()); }

  void background_and_pen_follow_a_palette_change() {
    HistoryChart chart;
    chart.set_series(series_of({{-30, 1.0}, {0, 3.0}}), 600s, QDateTime());
    auto* plot = chart.findChild<QCustomPlot*>();
    QCOMPARE(plot->axisRect()->backgroundBrush().color(), pychron::ui::plot_style().plot_background);
    pychron::ui::PlotStyle style = pychron::ui::default_plot_style();
    style.plot_background = QColor("#fafad2");
    style.series[0] = QColor("#123456");
    pychron::ui::set_plot_style(style);
    QCOMPARE(plot->axisRect()->backgroundBrush().color().name(), QStringLiteral("#fafad2"));
    QCOMPARE(graph_of(chart)->pen().color().name(), QStringLiteral("#123456"));
    QCOMPARE(chart.point_count(), 2);  // the data stays
  }

  void drawsThePointsOfTheRange() {
    HistoryChart chart;
    chart.set_series(series_of({{-30, 1.0}, {-20, 2.0}, {-15, kNaN}, {0, 3.0}}), 600s, QDateTime());
    QCOMPARE(chart.point_count(), 3);
    QCPGraph* graph = graph_of(chart);
    QVERIFY(graph != nullptr);
    // The break is kept as a NaN, which QCustomPlot draws as a gap: two segments.
    int nan = 0;
    for (auto it = graph->data()->constBegin(); it != graph->data()->constEnd(); ++it) nan += std::isnan(it->value);
    QCOMPARE(nan, 1);
  }

  void theXAxisIsTheRangeEndingAtTheNewest() {
    HistoryChart chart;
    const QDateTime newest = half_past_eight();
    chart.set_series(series_of({{-60, 1.0}, {0, 2.0}}), 600s, newest);
    const double right = key_of(newest);
    QCOMPARE(chart.shown_x().lo, right - 600.0);
    QCOMPARE(chart.shown_x().hi, right);
    QCOMPARE(chart.x_label_at(right), QString("08:30"));

    chart.set_series(series_of({{-60, 1.0}, {0, 2.0}}), 6h, newest);
    QCOMPARE(chart.shown_x().lo, right - 6 * 3600.0);
    QCOMPARE(chart.x_label_at(right), QString("10-13 08:30"));
  }

  void withoutAWallTimeTheAxisIsMinutesBeforeNow() {
    HistoryChart chart;
    chart.set_series(series_of({{-60, 1.0}, {0, 2.0}}), 600s, QDateTime());
    QCOMPARE(chart.shown_x().lo, -600.0);
    QCOMPARE(chart.shown_x().hi, 0.0);
    QCOMPARE(chart.x_label_at(-300.0), QString("-5"));
  }

  void yFitsWhatIsShownWithAMargin() {
    HistoryChart chart;
    chart.set_series(series_of({{-60, 2.0}, {0, 4.0}}), 600s, QDateTime());
    QVERIFY(chart.shown_y().lo < 2.0);
    QVERIFY(chart.shown_y().hi > 4.0);

    chart.set_series(series_of({{0, 5.0}}), 600s, QDateTime());
    QVERIFY(chart.shown_y().lo < 5.0);
    QVERIFY(chart.shown_y().hi > 5.0);

    chart.set_series(series_of({{0, 0.0}}), 600s, QDateTime());
    QVERIFY(chart.shown_y().lo < chart.shown_y().hi);
  }

  void logScaleNeverTakesANonPositiveRange() {
    HistoryChart chart;
    chart.set_log(true);
    QVERIFY(chart.log());
    chart.set_series(series_of({{-60, 1e-9}, {0, 1e-7}}), 600s, QDateTime());
    QVERIFY(chart.shown_y().lo > 0.0);
    QVERIFY(chart.shown_y().lo < 1e-9);
    QVERIFY(chart.shown_y().hi > 1e-7);
    QVERIFY(chart.shown_y().hi < 1e-6);  // a factor, not a difference, and no floor at 1e-6

    chart.set_series(series_of({{-60, 1e-9}, {-30, 0.0}, {0, 1e-7}}), 600s, QDateTime());
    QCOMPARE(chart.point_count(), 2);  // the zero is a break on log
    QVERIFY(chart.shown_y().lo > 0.0);

    chart.set_log(false);
    QCOMPARE(chart.point_count(), 3);  // and a point on linear
    QVERIFY(chart.shown_y().lo < 0.0);  // the linear range with its margin, not one cut to fit a log axis
    QVERIFY(chart.shown_y().hi > 1e-7);
  }

  void oneValueOnLogStillGivesARange() {
    HistoryChart chart;
    chart.set_log(true);
    chart.set_series(series_of({{0, 3e-8}}), 600s, QDateTime());
    QVERIFY(chart.shown_y().lo > 0.0);
    QVERIFY(chart.shown_y().lo < 3e-8);
    QVERIFY(chart.shown_y().hi > 3e-8);
  }

  void anInfiniteReadingIsABreak() {
    HistoryChart chart;
    const double inf = std::numeric_limits<double>::infinity();
    chart.set_series(series_of({{-60, 2.0}, {-30, inf}, {0, 4.0}}), 600s, QDateTime());
    QCOMPARE(chart.point_count(), 2);
    QVERIFY(std::isfinite(chart.shown_y().hi));
    QCOMPARE(chart.summary()->highest, 4.0);
  }

  void readingsOlderThanTheRangeAreLeftOut() {
    HistoryChart chart;
    chart.set_series(series_of({{-700, 9.0}, {-60, 2.0}, {0, 4.0}}), 600s, QDateTime());
    QCOMPARE(chart.point_count(), 2);
    QCOMPARE(chart.summary()->points, 2);
    QCOMPARE(chart.summary()->highest, 4.0);
  }

  void justUnderSixHoursHasNoDate() {
    HistoryChart chart;
    const QDateTime newest = half_past_eight();
    chart.set_series(series_of({{0, 1.0}}), 5h + 59min, newest);
    QCOMPARE(chart.x_label_at(key_of(newest)), QString("08:30"));
  }

  void compactHasNoAxisTitles() {
    HistoryChart chart;
    chart.set_series(series_of({{0, 1.0}}), 600s, QDateTime());
    auto* plot = chart.findChild<QCustomPlot*>();
    QCOMPARE(plot->yAxis->label(), QString("torr"));
    QVERIFY(!plot->xAxis->label().isEmpty());
    chart.set_compact(true);
    QVERIFY(plot->xAxis->label().isEmpty());
    QVERIFY(plot->yAxis->label().isEmpty());
  }

  void linearIsTheDefault() {
    HistoryChart chart;
    QVERIFY(!chart.log());
  }

  void guidesInsideTheViewAreDrawnAndOthersAreNot() {
    HistoryChart chart;
    chart.set_series(series_of({{-60, 2.0}, {0, 4.0}}), 600s, QDateTime());
    chart.set_guides({3.0, 100.0});
    QCOMPARE(chart.guide_count(), 1);
    chart.set_series(series_of({{-60, 50.0}, {0, 150.0}}), 600s, QDateTime());
    QCOMPARE(chart.guide_count(), 1);  // now 100 is inside and 3 is not
    chart.set_log(true);
    chart.set_guides({-1.0, 0.0, 100.0});
    QCOMPARE(chart.guide_count(), 1);  // nothing at or below zero on a log axis
    chart.set_guides({});
    QCOMPARE(chart.guide_count(), 0);
  }

  void anEmptySeriesHasNoSummaryAndSaysSo() {
    HistoryChart chart;
    chart.set_series(series_of({}), 600s, QDateTime());
    QVERIFY(!chart.summary().has_value());
    auto* stack = chart.findChild<QStackedWidget*>();
    QVERIFY(stack != nullptr);
    auto* label = qobject_cast<QLabel*>(stack->currentWidget());
    QVERIFY(label != nullptr);
    QCOMPARE(label->text(), QString("No readings"));
    QCOMPARE(label->alignment(), Qt::AlignCenter);

    chart.set_series(series_of({{0, 1.0}}), 600s, QDateTime());
    QVERIFY(qobject_cast<QCustomPlot*>(stack->currentWidget()) != nullptr);

    chart.set_series(series_of({{-30, kNaN}, {0, kNaN}}), 600s, QDateTime());  // only breaks
    QVERIFY(!chart.summary().has_value());
    QVERIFY(qobject_cast<QLabel*>(stack->currentWidget()) != nullptr);
  }

  void theSummaryIsOfTheRangeShown() {
    HistoryChart chart;
    chart.set_series(series_of({{-90, 7.0}, {-60, 2.0}, {-30, kNaN}, {0, 4.0}}), 600s, QDateTime());
    const auto summary = chart.summary();
    QVERIFY(summary.has_value());
    QCOMPARE(summary->points, 3);
    QCOMPARE(summary->latest, 4.0);
    QCOMPARE(summary->lowest, 2.0);
    QCOMPARE(summary->highest, 7.0);

    chart.set_series(series_of({{-60, 2.0}, {-30, 5.0}, {0, kNaN}}), 600s, QDateTime());
    QCOMPARE(chart.summary()->latest, 5.0);  // the newest reading that is a number
  }
};

QTEST_MAIN(TestHistoryChart)
#include "test_history_chart.moc"
