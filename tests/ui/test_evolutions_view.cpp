// EvolutionsView fed synthetic series and fits: curves to time zero,
// intercepts, outliers.

#include <cmath>
#include <limits>

#include <QScrollArea>
#include <QScrollBar>
#include <QtTest/QtTest>

#include <qcustomplot.h>

#include "evolutions_view.hpp"

using pychron::experiment::collect::FitsUpdated;
using pychron::experiment::collect::SeriesFit;
using pychron::experiment::collect::SeriesKey;
using pychron::experiment::collect::SeriesKind;
using pychron::experiment::collect::SeriesUpdated;
using pychron::ui::evolution_panels;
using pychron::ui::EvolutionsView;
namespace reduction = pychron::reduction;

class TestEvolutionsView : public QObject {
  Q_OBJECT

  static SeriesUpdated point(const SeriesKey& key, double t, double v) {
    SeriesUpdated u;
    u.label = "main";
    u.kind = key.kind;
    u.values.emplace_back(key, v);
    u.t = t;
    return u;
  }

 private slots:
  void drawsFitsToTimeZeroAndListsIntercepts() {
    EvolutionsView view({{"H1", QColor(Qt::red)}});
    view.on_run_started({0, "uuid", "66001", {}});
    const SeriesKey ar40{"Ar40", "H1", SeriesKind::Signal};
    // y = 100 + 2 (t - 10), one outlier at t = 13.
    reduction::Series s;
    std::vector<SeriesUpdated> batch;
    for (int i = 1; i <= 8; ++i) {
      const double t = 10 + i;
      const double v = i == 3 ? 160.0 : 100 + 2.0 * i;
      batch.push_back(point(ar40, t, v));
      s.x.push_back(t - 10);
      s.y.push_back(v);
    }
    view.on_series(batch);
    QCOMPARE(view.graph_count(), 1);
    QCOMPARE(view.fit_curve_count(), 0);  // no fit yet
    QVERIFY(view.intercept_rows().isEmpty());

    reduction::FitSpec spec{reduction::FitKind::Linear};
    spec.outliers = {true, 1, 2.0};
    auto fit = reduction::fit(s, spec);
    QVERIFY(fit.has_value());
    QCOMPARE(fit->filtered_idx, std::vector<std::size_t>{2});
    FitsUpdated fu;
    fu.label = "main";
    fu.kind = SeriesKind::Signal;
    fu.time_zero = 10;
    fu.fits.push_back(SeriesFit{ar40, *fit});
    view.on_fits({fu});
    QTRY_COMPARE(view.fit_curve_count(), 1);
    QVERIFY(view.fit_of(ar40).has_value());
    QVERIFY(std::abs(view.fit_of(ar40)->value - 100.0) < 1e-6);
    // A table, a row per series: isotope, detector, fit, intercept, error, percent, points.
    QCOMPARE(view.intercept_columns(),
             (QStringList{QStringLiteral("Isotope"), QStringLiteral("Detector"), QStringLiteral("Fit"),
                          QStringLiteral("Intercept"), QStringLiteral("± 1σ"), QStringLiteral("%"),
                          QStringLiteral("Points")}));
    QCOMPARE(view.intercept_rows().size(), 1);
    const QStringList row = view.intercept_rows().front();
    QCOMPARE(row.size(), 7);
    QCOMPARE(row.mid(0, 4), (QStringList{QStringLiteral("Ar40"), QStringLiteral("H1"), QStringLiteral("linear"),
                                         QStringLiteral("100")}));
    QCOMPARE(row[4], QString::number(fit->error, 'g', 3));
    QCOMPARE(row[5], QString::number(100.0 * fit->error / fit->value, 'f', 3));
    QCOMPARE(row[6], QStringLiteral("7"));  // eight points, one excluded

    // The x axis reaches back to time zero, where the intercept is.
    QTRY_VERIFY(view.panel_x_range(0).first <= 10.0);

    // Other kinds have their own fits; baselines have none here.
    view.set_kind(SeriesKind::Baseline);
    QCOMPARE(view.fit_curve_count(), 0);
    QVERIFY(view.intercept_rows().isEmpty());
    view.set_kind(SeriesKind::Signal);
    QCOMPARE(view.fit_curve_count(), 1);
    QCOMPARE(view.intercept_rows().size(), 1);

    // A second series is a panel of its own, below the first.
    const SeriesKey ar39{"Ar39", "H2", SeriesKind::Signal};
    view.on_series({point(ar39, 11, 5.0), point(ar39, 12, 5.1)});
    QCOMPARE(view.graph_count(), 2);
    QCOMPARE(view.panel_titles(), (QStringList{"Ar40 H1", "Ar39 H2"}));

    // A new run clears the fits.
    view.on_run_started({1, "uuid2", "66002", {}});
    QVERIFY(!view.fit_of(ar40).has_value());
    QCOMPARE(view.fit_curve_count(), 0);
  }

  // ---------------------------------------------------------------- the view's panels
  static std::vector<SeriesUpdated> readings(const SeriesKey& key, std::initializer_list<std::pair<double, double>> tv) {
    std::vector<SeriesUpdated> out;
    for (const auto& [t, v] : tv) out.push_back(point(key, t, v));
    return out;
  }
  static bool holds(std::pair<double, double> range, double v) { return range.first <= v && v <= range.second; }

  void onePanelPerSeriesOfTheShownKind() {
    EvolutionsView view;
    view.on_series(readings({"Ar40", "H1", SeriesKind::Signal}, {{1, 10}}));
    view.on_series(readings({"Ar39", "AX", SeriesKind::Signal}, {{1, 5}}));
    view.on_series(readings({"Ar36", "CDD", SeriesKind::Signal}, {{1, 1}}));
    view.on_series(readings({"", "H1", SeriesKind::Baseline}, {{1, 0.01}}));
    QCOMPARE(view.graph_count(), 3);
    QCOMPARE(view.panel_titles().size(), 3);
    view.set_kind(SeriesKind::Baseline);
    QCOMPARE(view.graph_count(), 1);
    QCOMPARE(view.panel_titles(), QStringList{"H1"});
  }

  void aNewSeriesIsAddedAtTheBottom() {
    EvolutionsView view;
    view.on_series(readings({"Ar40", "H1", SeriesKind::Signal}, {{1, 10}}));
    view.on_series(readings({"Ar36", "CDD", SeriesKind::Signal}, {{1, 1}}));
    QCOMPARE(view.panel_titles(), (QStringList{"Ar40 H1", "Ar36 CDD"}));
    view.on_series(readings({"Ar39", "AX", SeriesKind::Signal}, {{2, 5}}));
    QCOMPARE(view.panel_titles(), (QStringList{"Ar40 H1", "Ar36 CDD", "Ar39 AX"}));
  }

  void eachPanelScalesItsOwnY() {
    EvolutionsView view;
    view.on_series(readings({"Ar40", "H1", SeriesKind::Signal}, {{1, 999}, {2, 1000}, {3, 1001}}));
    view.on_series(readings({"Ar36", "CDD", SeriesKind::Signal}, {{1, 0.9}, {2, 1.0}, {3, 1.1}}));
    // Waits for a refresh: before one, a panel has QCustomPlot's default range (0 to 5).
    QTRY_VERIFY(holds(view.panel_y_range(0), 999) && holds(view.panel_y_range(0), 1001));
    QVERIFY(holds(view.panel_y_range(1), 0.9) && holds(view.panel_y_range(1), 1.1));
    QVERIFY(view.panel_y_range(1).second < 2);  // its own scale, not Ar40's
    QVERIFY(view.panel_y_range(0).first > 900);
  }

  void everyPanelSharesTheTimeAxis() {
    EvolutionsView view;
    view.on_series(readings({"Ar40", "H1", SeriesKind::Signal}, {{10, 1}, {20, 2}}));
    view.on_series(readings({"Ar36", "CDD", SeriesKind::Signal}, {{15, 1}, {30, 2}}));
    QTRY_VERIFY(view.panel_x_range(0).first <= 10 && view.panel_x_range(0).second >= 30);
    QCOMPARE(view.panel_x_range(1), view.panel_x_range(0));
  }

  void onlyTheBottomPanelHasTimeLabels() {
    EvolutionsView view;
    for (const char* iso : {"Ar40", "Ar39", "Ar36"}) view.on_series(readings({iso, "H1", SeriesKind::Signal}, {{1, 1}}));
    QCOMPARE(view.graph_count(), 3);
    QVERIFY(!view.panel_has_time_labels(0));
    QVERIFY(!view.panel_has_time_labels(1));
    QVERIFY(view.panel_has_time_labels(2));
  }

  void aRunStartingEmptiesThePanels() {
    EvolutionsView view;
    view.on_series(readings({"Ar40", "H1", SeriesKind::Signal}, {{1, 10}}));
    view.on_series(readings({"Ar36", "CDD", SeriesKind::Signal}, {{1, 1}}));
    view.on_run_started({1, "uuid", "66002", {}});
    QCOMPARE(view.graph_count(), 0);
    QVERIFY(view.panel_titles().isEmpty());
    view.on_series(readings({"Ar36", "CDD", SeriesKind::Signal}, {{1, 1}}));
    QCOMPARE(view.panel_titles(), QStringList{"Ar36 CDD"});
  }

  void aFlatSeriesStillHasARange() {
    EvolutionsView view;
    view.on_series(readings({"", "H1", SeriesKind::Signal}, {{1, 200.0}, {2, 200.0}, {3, 200.0}}));
    QTRY_VERIFY(holds(view.panel_y_range(0), 200.0));
    QVERIFY(view.panel_y_range(0).second > view.panel_y_range(0).first);
  }

  void aKindWithoutSeriesShowsNoPanels() {
    EvolutionsView view;
    view.on_series(readings({"Ar40", "H1", SeriesKind::Signal}, {{1, 10}}));
    view.on_series(readings({"Ar36", "CDD", SeriesKind::Signal}, {{1, 1}}));
    view.set_kind(SeriesKind::Sniff);
    QCOMPARE(view.graph_count(), 0);
    QVERIFY(view.panel_titles().isEmpty());
    view.set_kind(SeriesKind::Signal);
    QCOMPARE(view.graph_count(), 2);
  }

  void aReadingThatIsNoNumberLeavesTheRangeFinite() {
    EvolutionsView view;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    view.on_series(readings({"Ar40", "H1", SeriesKind::Signal}, {{1, 10.0}, {2, nan}, {3, 30.0}}));
    QTRY_VERIFY(holds(view.panel_y_range(0), 10.0) && holds(view.panel_y_range(0), 30.0));
    QVERIFY(std::isfinite(view.panel_y_range(0).first) && std::isfinite(view.panel_y_range(0).second));
  }

  void aSeriesOfAnotherKindLeavesTheShownPanels() {
    EvolutionsView view;
    view.on_series(readings({"Ar40", "H1", SeriesKind::Signal}, {{1, 10}}));
    view.on_series(readings({"Ar36", "CDD", SeriesKind::Signal}, {{1, 1}}));
    const QStringList shown = view.panel_titles();
    view.on_series(readings({"", "H1", SeriesKind::Baseline}, {{5, 0.01}}));
    QCOMPARE(view.panel_titles(), shown);
    view.set_kind(SeriesKind::Baseline);
    QCOMPARE(view.panel_titles(), QStringList{"H1"});
  }

  void aNewSeriesIsScaledAtOnce() {
    EvolutionsView view;
    view.on_series(readings({"Ar40", "H1", SeriesKind::Signal}, {{1, 999}, {2, 1001}}));
    // No wait for the refresh timer: a panel is never shown at QCustomPlot's default 0 to 5.
    QVERIFY(holds(view.panel_y_range(0), 999) && holds(view.panel_y_range(0), 1001));
    view.on_series(readings({"Ar36", "CDD", SeriesKind::Signal}, {{1, 100}, {2, 101}}));
    QVERIFY(holds(view.panel_y_range(1), 100) && holds(view.panel_y_range(1), 101));
    QVERIFY(holds(view.panel_y_range(0), 999));
  }

  void theBottomPanelIsAsTallAsTheOthers() {
    EvolutionsView view;
    view.resize(400, 900);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    for (const char* iso : {"Ar40", "Ar39", "Ar38", "Ar37"}) view.on_series(readings({iso, "H1", SeriesKind::Signal}, {{1, 1}}));
    QCOMPARE(view.graph_count(), 4);
    // The plotting area of each panel, its time axis aside: settles within a few refreshes.
    const auto even = [&] {
      const int top = view.panel_plot_height(0);
      return top > 0 && std::abs(view.panel_plot_height(3) - top) <= top / 10;
    };
    QTRY_VERIFY2(even(), qPrintable(QString("%1 vs %2").arg(view.panel_plot_height(3)).arg(view.panel_plot_height(0))));
  }

  void manyPanelsScroll() {
    EvolutionsView view;
    view.resize(400, 700);  // the plot's area at least its 200 px floor, under tabs, title and tables
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    view.on_series(readings({"Ar40", "H1", SeriesKind::Signal}, {{1, 10}}));
    view.on_series(readings({"Ar36", "CDD", SeriesKind::Signal}, {{1, 1}}));
    QCOMPARE(view.graph_count(), 2);
    QTRY_COMPARE(view.scroll_area()->verticalScrollBar()->maximum(), 0);  // two fit
    for (int i = 0; i < 8; ++i)
      view.on_series(readings({"Ar" + std::to_string(30 + i), "L" + std::to_string(i), SeriesKind::Signal}, {{1, 1}}));
    QCOMPARE(view.graph_count(), 10);
    QTRY_VERIFY(view.scroll_area()->verticalScrollBar()->maximum() > 0);
    QVERIFY(view.plot()->height() >= 10 * EvolutionsView::kPanelMinHeight);
    QCOMPARE(view.scroll_area()->horizontalScrollBar()->maximum(), 0);
  }

  // ---------------------------------------------------------------- panels
  static QStringList titles(const std::vector<pychron::ui::EvolutionPanel>& panels) {
    QStringList out;
    for (const auto& p : panels) out << p.title;
    return out;
  }

  void panelsComeInArrivalOrder() {
    const std::vector<SeriesKey> arrived{{"Ar40", "H1", SeriesKind::Signal},
                                         {"Ar36", "CDD", SeriesKind::Signal},
                                         {"Ar39", "AX", SeriesKind::Signal}};
    const auto panels = evolution_panels(arrived, SeriesKind::Signal);
    QCOMPARE(titles(panels), (QStringList{"Ar40 H1", "Ar36 CDD", "Ar39 AX"}));
    QCOMPARE(panels.size(), std::size_t{3});
    for (std::size_t i = 0; i < panels.size(); ++i) QVERIFY(panels[i].key == arrived[i]);
  }

  void anIsotopeOnTwoDetectorsHasTwoPanels() {
    const auto panels = evolution_panels({{"Ar40", "H1", SeriesKind::Signal}, {"Ar40", "H2", SeriesKind::Signal}},
                                         SeriesKind::Signal);
    QCOMPARE(titles(panels), (QStringList{"Ar40 H1", "Ar40 H2"}));
  }

  void baselinePanelsAreTitledByDetector() {
    QCOMPARE(titles(evolution_panels({{"", "H1", SeriesKind::Baseline}}, SeriesKind::Baseline)), QStringList{"H1"});
    QCOMPARE(titles(evolution_panels({{"Ar40", "H1", SeriesKind::Baseline}}, SeriesKind::Baseline)),
             QStringList{"Ar40 H1"});
  }

  void sniffPanelsAreTitledByIsotope() {
    QCOMPARE(titles(evolution_panels({{"Ar40", "H1", SeriesKind::Sniff}}, SeriesKind::Sniff)), QStringList{"Ar40"});
  }

  void eachKindKeepsItsOwnOrder() {
    const std::vector<SeriesKey> arrived{{"", "H1", SeriesKind::Baseline},
                                         {"Ar40", "H1", SeriesKind::Signal},
                                         {"", "AX", SeriesKind::Baseline},
                                         {"Ar39", "AX", SeriesKind::Signal}};
    QCOMPARE(titles(evolution_panels(arrived, SeriesKind::Signal)), (QStringList{"Ar40 H1", "Ar39 AX"}));
    QCOMPARE(titles(evolution_panels(arrived, SeriesKind::Baseline)), (QStringList{"H1", "AX"}));
    QVERIFY(evolution_panels(arrived, SeriesKind::Sniff).empty());
  }

  void aFitBeforeItsPointsIsIgnored() {
    EvolutionsView view;
    FitsUpdated fu;
    fu.fits.push_back(SeriesFit{{"Ar39", "H2", SeriesKind::Signal}, reduction::Intercept{}});
    view.on_fits({fu});
    QVERIFY(!view.fit_of({"Ar39", "H2", SeriesKind::Signal}).has_value());
  }
};

QTEST_MAIN(TestEvolutionsView)
#include "test_evolutions_view.moc"
