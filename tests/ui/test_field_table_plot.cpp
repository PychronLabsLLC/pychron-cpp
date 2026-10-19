// FieldTablePlot: one detector column of a field table, value against mass,
// with its fit curve over the detector's mass range widened by
// FieldTable::kInverseMargin on each side.

#include <QtTest/QtTest>

#include "field_table_plot.hpp"
#include "plot_style.hpp"

using pychron::spectrometer::ControlPoint;
using pychron::spectrometer::FieldTable;
using pychron::spectrometer::FitKind;
using pychron::spectrometer::TableAxis;
using pychron::ui::FieldTablePlot;

namespace {

FieldTable table(FitKind fit) {
  return FieldTable(fit, TableAxis::Dac,
                    {ControlPoint{"Ar36", 35.967545105, {{"H1", 4.505}}},
                     ControlPoint{"Ar39", 38.964313, {{"H1", 4.874}, {"AX", 4.75}}},
                     ControlPoint{"Ar40", 39.9623831237, {{"H1", 5.001}, {"AX", 4.874}}}});
}

}  // namespace

class TestFieldTablePlot : public QObject {
  Q_OBJECT

 private slots:
  void cleanup() { pychron::ui::set_plot_style(pychron::ui::default_plot_style()); }

  void pointsAndCurveCoverTheMarginedRange() {
    FieldTablePlot plot;
    plot.show_detector(table(FitKind::Linear), "H1");
    QCOMPARE(plot.point_count(), 3);
    QCOMPARE(plot.curve_sample_count(), 200);
    const auto [lo, hi] = plot.curve_mass_range();
    QCOMPARE(lo, 35.967545105 - FieldTable::kInverseMargin);
    QCOMPARE(hi, 39.9623831237 + FieldTable::kInverseMargin);
    // Only the points that have a value on the detector.
    plot.show_detector(table(FitKind::Linear), "AX");
    QCOMPARE(plot.point_count(), 2);
    QCOMPARE(plot.curve_sample_count(), 200);
  }

  void discreteDrawsPointsOnly() {
    FieldTablePlot plot;
    plot.show_detector(table(FitKind::Discrete), "H1");
    QCOMPARE(plot.point_count(), 3);
    QCOMPARE(plot.curve_sample_count(), 0);
    QCOMPARE(plot.point_tooltips().front(), QString("Ar36: value 4.50500"));
  }

  void failedFitDrawsPointsOnly() {
    FieldTablePlot plot;
    // Cubic needs four points.
    plot.show_detector(table(FitKind::Cubic), "H1");
    QCOMPARE(plot.point_count(), 3);
    QCOMPARE(plot.curve_sample_count(), 0);
    // A detector with no points at all, or none chosen.
    plot.show_detector(table(FitKind::Linear), "L2");
    QCOMPARE(plot.point_count(), 0);
    QCOMPARE(plot.curve_sample_count(), 0);
    plot.show_detector(FieldTable{}, "");
    QCOMPARE(plot.point_count(), 0);
  }

  void tooltipsCarryResiduals() {
    FieldTablePlot plot;
    const FieldTable t = table(FitKind::Linear);
    plot.show_detector(t, "H1");
    const QStringList tips = plot.point_tooltips();
    QCOMPARE(tips.size(), 3);
    const double residual = 4.874 - t.value_for(38.964313, "H1").value();
    QCOMPARE(tips[1], QString("Ar39: value 4.87400, residual %1").arg(residual, 0, 'f', 5));
  }

  void restylesOnPlotStyleChange() {
    FieldTablePlot plot;
    plot.show_detector(table(FitKind::Linear), "H1");
    QCOMPARE(plot.point_color(), pychron::ui::plot_series_color(0));
    auto style = pychron::ui::default_plot_style();
    style.series.front() = QColor(QStringLiteral("#123456"));
    pychron::ui::set_plot_style(style);
    QCOMPARE(plot.point_color(), QColor(QStringLiteral("#123456")));
    QCOMPARE(plot.curve_color(), QColor(QStringLiteral("#123456")));
  }
};

QTEST_MAIN(TestFieldTablePlot)
#include "test_field_table_plot.moc"
