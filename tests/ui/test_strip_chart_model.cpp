// StripChartModel and IntensitiesModel: pure data, no widgets.

#include <chrono>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>

#include <QtTest/QtTest>

#include "intensities_model.hpp"
#include "strip_chart_model.hpp"
#include "theme.hpp"

using namespace pychron;
using namespace std::chrono_literals;
using pychron::spectrometer::IntensityReading;
using pychron::spectrometer::Value;
using pychron::ui::AxisRange;
using pychron::ui::DetectorSeries;
using pychron::ui::IntensitiesModel;
using pychron::ui::StripChartModel;
using pychron::ui::YScale;

namespace {

using Cell = std::pair<std::string, std::optional<double>>;

IntensityReading row(double t_s, std::initializer_list<Cell> cells, bool saturated = false) {
  IntensityReading r;
  r.reading.ts = TimePoint{} + std::chrono::duration_cast<Duration>(std::chrono::duration<double>(t_s));
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

std::vector<DetectorSeries> two() {
  DetectorSeries a;
  a.name = "H1";
  DetectorSeries b;
  b.name = "AX";
  return {a, b};
}

constexpr TimePoint kT0{};

}  // namespace

class TestStripChartModel : public QObject {
  Q_OBJECT

 private slots:
  void xIsSecondsSinceFirstReading() {
    StripChartModel m(two());
    m.append(row(100.0, {{"H1", 1.0}}));
    m.append(row(102.5, {{"H1", 2.0}}));
    QCOMPARE(m.series(0).size(), std::size_t{2});
    QCOMPARE(m.series(0)[0].t, 0.0);
    QCOMPARE(m.series(0)[1].t, 2.5);
    QCOMPARE(m.latest_x(), 2.5);
  }

  void xRangeBeforeAndAfterWindowFills() {
    StripChartModel m(two());
    QCOMPARE(m.latest_x(), 0.0);
    m.append(row(0.0, {{"H1", 1.0}}));
    m.append(row(30.0, {{"H1", 1.0}}));
    QCOMPARE(m.x_range().lo, 0.0);
    QVERIFY(qFuzzyCompare(m.x_range().hi, 63.0));
    m.append(row(100.0, {{"H1", 1.0}}));
    QVERIFY(qFuzzyCompare(m.x_range().lo, 40.0));
    QVERIFY(qFuzzyCompare(m.x_range().hi, 103.0));
  }

  void ringSpanFollowsScanWidth() {
    StripChartModel m(two());
    m.set_scan_width(10.0);
    for (int s = 0; s <= 30; ++s) m.append(row(s, {{"H1", 1.0}}));
    QCOMPARE(m.series(0)[0].t, 12.0);  // 30 - 1.8 * 10
    QCOMPARE(m.series(0).size(), std::size_t{19});
  }

  void scanWidthClampedToOneSecond() {
    StripChartModel m(two());
    QCOMPARE(m.scan_width(), 60.0);
    m.set_scan_width(0.2);
    QCOMPARE(m.scan_width(), 1.0);
    m.set_scan_width(-5.0);
    QCOMPARE(m.scan_width(), 1.0);
  }

  void autoscalePadsTenPercent() {
    StripChartModel m(two());
    for (int s = 0; s <= 10; ++s) m.append(row(s, {{"H1", 10.0 + s}}));
    const AxisRange r = m.y_range(kT0);
    QVERIFY(qFuzzyCompare(r.lo, 9.0));
    QVERIFY(qFuzzyCompare(r.hi, 21.0));
  }

  void autoscaleFlatSeriesPads() {
    StripChartModel m(two());
    for (int s = 0; s < 3; ++s) m.append(row(s, {{"H1", 5.0}}));
    const AxisRange r = m.y_range(kT0);
    QVERIFY(r.lo < 5.0);
    QVERIFY(r.hi > 5.0);
  }

  void autoscaleThrottledToHalfSecond() {
    StripChartModel m(two());
    m.append(row(0.0, {{"H1", 10.0}}));
    m.append(row(1.0, {{"H1", 20.0}}));
    const AxisRange first = m.y_range(kT0);
    m.append(row(2.0, {{"H1", 100.0}}));
    QCOMPARE(m.y_range(kT0 + 400ms).hi, first.hi);
    QVERIFY(m.y_range(kT0 + 600ms).hi > 100.0);
  }

  void autoscaleIgnoresHiddenSeries() {
    StripChartModel m(two());
    m.append(row(0.0, {{"H1", 10.0}, {"AX", 1000.0}}));
    m.append(row(1.0, {{"H1", 20.0}, {"AX", 2000.0}}));
    m.set_visible(1, false);
    QVERIFY(m.y_range(kT0).hi < 100.0);
  }

  void hiddenSeriesStillBuffers() {
    StripChartModel m(two());
    m.set_visible(1, false);
    m.append(row(0.0, {{"AX", 1.0}}));
    m.append(row(1.0, {{"AX", 2.0}}));
    QCOMPARE(m.series(1).size(), std::size_t{2});
    m.set_visible(1, true);
    QVERIFY(m.y_range(kT0).hi > 2.0);
  }

  void nulloptLeavesGapAndAdvancesTime() {
    StripChartModel m(two());
    m.append(row(0.0, {{"H1", 1.0}}));
    m.append(row(1.0, {{"H1", std::nullopt}}));
    m.append(row(2.0, {{"H1", 3.0}}));
    QCOMPARE(m.series(0).size(), std::size_t{3});
    QVERIFY(std::isnan(m.series(0)[1].value));
    QCOMPARE(m.series(0)[1].t, 1.0);
    const auto rg = m.series(0).range(0.0, 2.0);
    QVERIFY(rg);
    QCOMPARE(rg->first, 1.0);
    QCOMPARE(rg->second, 3.0);

    m.append(row(5.0, {{"H1", std::nullopt}, {"AX", std::nullopt}}));
    QCOMPARE(m.latest_x(), 5.0);
  }

  void allNulloptReadingKeepsPreviousYRange() {
    StripChartModel m(two());
    m.append(row(0.0, {{"H1", 10.0}}));
    m.append(row(1.0, {{"H1", 20.0}}));
    const AxisRange before = m.y_range(kT0);
    m.append(row(2.0, {{"H1", std::nullopt}, {"AX", std::nullopt}}));
    const AxisRange after = m.y_range(kT0 + 1s);
    QCOMPARE(after.lo, before.lo);
    QCOMPARE(after.hi, before.hi);

    StripChartModel empty(two());
    const AxisRange none = empty.y_range(kT0);
    QCOMPARE(empty.y_range(kT0 + 1s).lo, none.lo);
  }

  void manualLimitsRejectInverted() {
    StripChartModel m(two());
    m.set_autoscale(false);
    QVERIFY(m.set_manual_y(1.0, 5.0));
    QVERIFY(!m.set_manual_y(5.0, 1.0));
    QVERIFY(!m.set_manual_y(2.0, 2.0));
    const AxisRange r = m.y_range(kT0);
    QCOMPARE(r.lo, 1.0);
    QCOMPARE(r.hi, 5.0);
    m.set_scale(YScale::Log);
    QVERIFY(!m.set_manual_y(0.0, 5.0));
    QVERIFY(m.set_manual_y(0.5, 5.0));
  }

  void logScaleClampsLowerLimit() {
    StripChartModel m(two());
    m.set_scale(YScale::Log);
    const double vals[] = {-1.0, 0.0, 2.0, 50.0};
    for (int i = 0; i < 4; ++i) m.append(row(i, {{"H1", vals[i]}}));
    const AxisRange r = m.y_range(kT0);
    QVERIFY(!std::isnan(r.lo) && !std::isnan(r.hi));
    QVERIFY(r.lo > 0.0);
    QVERIFY(qFuzzyCompare(r.lo, 2.0 * 0.9));
    QVERIFY(r.hi > 50.0);

    StripChartModel neg(two());
    neg.set_scale(YScale::Log);
    neg.append(row(0.0, {{"H1", -3.0}}));
    neg.append(row(1.0, {{"H1", 0.0}}));
    const AxisRange n = neg.y_range(kT0);
    QCOMPARE(n.lo, 1e-6);
    QVERIFY(n.hi > n.lo);
  }

  void clearResetsOrigin() {
    StripChartModel m(two());
    m.append(row(10.0, {{"H1", 1.0}}));
    m.append(row(12.0, {{"H1", 1.0}}));
    m.clear();
    QCOMPARE(m.series(0).size(), std::size_t{0});
    QCOMPARE(m.latest_x(), 0.0);
    m.append(row(20.0, {{"H1", 1.0}}));
    QCOMPARE(m.series(0)[0].t, 0.0);
    QCOMPARE(m.latest_x(), 0.0);
  }

  void backwardsTimestampResets() {
    StripChartModel m(two());
    m.append(row(10.0, {{"H1", 1.0}}));
    m.append(row(12.0, {{"H1", 1.0}}));
    m.append(row(5.0, {{"H1", 7.0}}));
    QCOMPARE(m.series(0).size(), std::size_t{1});
    QCOMPARE(m.series(0)[0].t, 0.0);
    QCOMPARE(m.series(0)[0].value, 7.0);
    QCOMPARE(m.latest_x(), 0.0);
  }

  void unknownDetectorInReadingIgnored() {
    StripChartModel m(two());
    m.append(row(0.0, {{"H1", 1.0}, {"NOPE", 9.0}}));
    QCOMPARE(m.series(0).size(), std::size_t{1});
    QCOMPARE(m.series(1).size(), std::size_t{0});
    QCOMPARE(m.detectors().size(), std::size_t{2});
  }

  void paletteUsedWhenColorUnset() {
    DetectorSeries a;
    a.name = "A";
    DetectorSeries b;
    b.name = "B";
    b.color = QColor(1, 2, 3);
    StripChartModel m({a, b});
    QCOMPARE(m.detectors()[0].color, StripChartModel::palette_color(0));
    QCOMPARE(m.detectors()[1].color, QColor(1, 2, 3));
    QVERIFY(StripChartModel::palette_color(0) != StripChartModel::palette_color(1));
    QCOMPARE(StripChartModel::palette_color(8), StripChartModel::palette_color(0));
  }

  // IntensitiesModel

  void sigmaOverElevenValuesMatchesHandCalc() {
    IntensitiesModel m(two());
    for (int v = 1; v <= 11; ++v) m.update(row(v, {{"H1", static_cast<double>(v)}}));
    QCOMPARE(m.data(m.index(0, IntensitiesModel::ColIntensity)).toString(), QStringLiteral("11.00000"));
    QCOMPARE(m.data(m.index(0, IntensitiesModel::ColSigma)).toString(), QStringLiteral("3.16228"));
  }

  void sigmaWithOneValueIsZero() {
    IntensitiesModel m(two());
    m.update(row(0.0, {{"H1", 4.0}}));
    QCOMPARE(m.data(m.index(0, IntensitiesModel::ColSigma)).toString(), QStringLiteral("0.00000"));
  }

  void saturatedCellRedWithTooltip() {
    IntensitiesModel m(two());
    m.update(row(0.0, {{"H1", 4.0}}, true));
    const QModelIndex cell = m.index(0, IntensitiesModel::ColIntensity);
    QCOMPARE(m.data(cell, Qt::BackgroundRole).value<QBrush>().color(), pychron::ui::theme().error);
    QCOMPARE(m.data(cell, Qt::ToolTipRole).toString(), QStringLiteral("saturated"));
    QCOMPARE(m.data(m.index(0, IntensitiesModel::ColColour), Qt::BackgroundRole).value<QBrush>().color(),
             StripChartModel::palette_color(0));
    m.update(row(1.0, {{"H1", 4.0}}));
    QVERIFY(!m.data(cell, Qt::BackgroundRole).isValid());
    QVERIFY(!m.data(cell, Qt::ToolTipRole).isValid());
  }

  void missingValueShowsEmDash() {
    IntensitiesModel m(two());
    m.update(row(0.0, {{"H1", 4.0}}));
    m.update(row(1.0, {{"H1", std::nullopt}}));
    QCOMPARE(m.data(m.index(0, IntensitiesModel::ColIntensity)).toString(), QStringLiteral("—"));
    QCOMPARE(m.data(m.index(0, IntensitiesModel::ColSigma)).toString(), QStringLiteral("—"));
    QCOMPARE(m.data(m.index(1, IntensitiesModel::ColIntensity)).toString(), QStringLiteral("—"));
  }

  void setIsotopeUpdatesColumn() {
    IntensitiesModel m(two());
    QSignalSpy spy(&m, &QAbstractItemModel::dataChanged);
    m.set_isotope("AX", QStringLiteral("Ar40"));
    QCOMPARE(m.data(m.index(1, IntensitiesModel::ColIsotope)).toString(), QStringLiteral("Ar40"));
    QCOMPARE(spy.count(), 1);
  }
};

QTEST_APPLESS_MAIN(TestStripChartModel)
#include "test_strip_chart_model.moc"
