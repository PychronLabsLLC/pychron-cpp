// PlotStyle: the live plots' series colours and background, by default the
// figures' palette on white (plot palette spec 4.1, 4.2, rule 1).

#include <cstddef>
#include <vector>

#include <QtTest/QtTest>

#include <pychron/processing/scene.hpp>
#include <qcustomplot.h>

#include "plot_style.hpp"
#include "strip_chart_model.hpp"
#include "strip_chart_view.hpp"

using pychron::ui::default_plot_style;
using pychron::ui::DetectorSeries;
using pychron::ui::plot_series_color;
using pychron::ui::plot_style;
using pychron::ui::StripChartModel;
using pychron::ui::StripChartView;
namespace pp = pychron::processing;

namespace {

QColor qcolor(const pp::Color& c) { return {c.r, c.g, c.b}; }

}  // namespace

class TestPlotStyle : public QObject {
  Q_OBJECT

 private slots:
  void default_series_are_the_figure_palette() {
    const auto style = default_plot_style();
    const auto palette = pp::default_palette();
    QCOMPARE(style.series.size(), std::size_t{12});
    QCOMPARE(palette.size(), std::size_t{12});
    for (std::size_t i = 0; i < palette.size(); ++i) QCOMPARE(style.series[i], qcolor(palette[i]));
  }

  void default_background_is_white() {
    QCOMPARE(default_plot_style().plot_background, QColor(QStringLiteral("#ffffff")));
  }

  void current_style_starts_as_default() { QVERIFY(plot_style() == default_plot_style()); }

  void series_color_cycles_twelve() {
    QCOMPARE(plot_series_color(12), plot_series_color(0));
    QCOMPARE(plot_series_color(6).name(), QStringLiteral("#e377c2"));
  }

  // Spec 5, rule 1: with nothing set, series n of a live plot and group n of a
  // figure are drawn alike.
  void a_live_plot_and_a_figure_draw_series_n_alike() {
    for (int n = 0; n < 12; ++n)
      QCOMPARE(StripChartModel::palette_color(static_cast<std::size_t>(n)), qcolor(pp::palette_color(n)));
  }

  void strip_chart_background_is_the_plot_style() {
    DetectorSeries d;
    d.name = "H1";
    StripChartModel model({d});
    const StripChartView view(model);
    QCOMPARE(view.plot()->axisRect()->backgroundBrush().color(), plot_style().plot_background);
  }
};

QTEST_MAIN(TestPlotStyle)
#include "test_plot_style.moc"
