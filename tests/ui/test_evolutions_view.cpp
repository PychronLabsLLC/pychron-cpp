// EvolutionsView fed synthetic series and fits: curves to time zero,
// intercepts, outliers; and its Peak center tab.

#include <cmath>
#include <limits>

#include <QListWidget>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabBar>
#include <QTableWidget>
#include <QtTest/QtTest>

#include <qcustomplot.h>

#include "evolutions_view.hpp"
#include "peak_center_view.hpp"
#include "plot_style.hpp"

using pychron::experiment::collect::FitsUpdated;
using pychron::experiment::collect::SeriesFit;
using pychron::experiment::collect::SeriesKey;
using pychron::experiment::collect::kBaseline;
using pychron::experiment::collect::kSignal;
using pychron::experiment::collect::kSniff;
using pychron::experiment::collect::SeriesUpdated;
using pychron::ui::evolution_panels;
using pychron::ui::EvolutionsView;
namespace reduction = pychron::reduction;
namespace jobs = pychron::jobs;

// A series key at the plan's first place (gas "", segment 0, position 0).
static SeriesKey skey(std::string label, std::string detector, std::string_view kind) {
  SeriesKey k;
  k.label = std::move(label);
  k.detector = std::move(detector);
  k.kind = std::string(kind);
  return k;
}

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

  // ---------------------------------------------------------------- peak centers
  static constexpr int kPeakCenterTab = 3;

  static jobs::SweepPoint sweep_point(double x, double h1) {
    jobs::SweepPoint p;
    p.x = x;
    p.y["H1"] = h1;
    return p;
  }
  static jobs::PeakCenterTryStarted try_started(int index) {
    jobs::PeakCenterTryStarted e;
    e.detector = "H1";
    e.isotope = "Ar40";
    e.try_index = index;
    e.n_tries = 2;
    e.start = 4.9;
    e.end = 5.1;
    e.initial_center = 5.0;
    return e;
  }
  static jobs::PeakCenterDone done(double center) {
    jobs::PeakCenterDone d;
    auto& r = d.result;
    r.ok = true;
    r.detector = "H1";
    r.isotope = "Ar40";
    r.initial_center = 5.0;
    r.center = center;
    r.config.n_tries = 1;
    jobs::PeakCenterTry t;
    t.points = {sweep_point(center - 0.01, 0), sweep_point(center, 100), sweep_point(center + 0.01, 0)};
    r.tries.push_back(t);
    return d;
  }
  static QTabBar* tab_bar(const EvolutionsView& view) { return view.findChild<QTabBar*>(); }
  static QListWidget* peak_list(const EvolutionsView& view) { return view.findChild<QListWidget*>(); }
  static void click_row(QListWidget* list, int row) {
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualItemRect(list->item(row)).center());
  }
  // The kind page: its panels and the intercepts table.
  static bool kind_page_shown(const EvolutionsView& view) {
    return view.scroll_area()->isVisibleTo(&view) && view.findChild<QTableWidget*>()->isVisibleTo(&view);
  }

 private slots:
  void drawsFitsToTimeZeroAndListsIntercepts() {
    EvolutionsView view({{"H1", QColor(Qt::red)}});
    view.on_run_started({0, "uuid", "66001", {}});
    const SeriesKey ar40 = skey("Ar40", "H1", kSignal);
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
    fu.kind = kSignal;
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
    view.set_kind(kBaseline);
    QCOMPARE(view.fit_curve_count(), 0);
    QVERIFY(view.intercept_rows().isEmpty());
    view.set_kind(kSignal);
    QCOMPARE(view.fit_curve_count(), 1);
    QCOMPARE(view.intercept_rows().size(), 1);

    // A second series is a panel of its own, below the first.
    const SeriesKey ar39 = skey("Ar39", "H2", kSignal);
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

  // ---------------------------------------------------------------- plot colours
  void cleanup() { pychron::ui::set_plot_style(pychron::ui::default_plot_style()); }

  static pychron::ui::PlotStyle changed_style() {
    pychron::ui::PlotStyle style = pychron::ui::default_plot_style();
    style.plot_background = QColor("#fafad2");
    for (std::size_t i = 0; i < style.series.size(); ++i) style.series[i] = QColor::fromHsv(static_cast<int>(i) * 25, 200, 150);
    return style;
  }
  // The pen of panel `i`'s points: a panel is four graphs, points first.
  static QColor panel_pen(const EvolutionsView& view, int i) { return view.plot()->graph(4 * i)->pen().color(); }

  void palette_change_recolors_open_panels() {
    EvolutionsView view({}, {{"AX", 2}});
    const SeriesKey ar39 = skey("Ar39", "AX", kSignal);
    view.on_series(readings(ar39, {{1, 5}, {2, 6}}));
    QCOMPARE(view.series_color(ar39), pychron::ui::plot_series_color(2));
    pychron::ui::set_plot_style(changed_style());
    QCOMPARE(view.series_color(ar39).name(), QColor::fromHsv(50, 200, 150).name());
    QCOMPARE(panel_pen(view, 0).name(), QColor::fromHsv(50, 200, 150).name());
    QCOMPARE(view.plot()->axisRect(0)->backgroundBrush().color().name(), QStringLiteral("#fafad2"));
    QCOMPARE(view.point_count(kSignal), std::size_t{2});  // the points stay
  }

  void a_config_color_survives_a_palette_change() {
    EvolutionsView view({{"H1", QColor("#123456")}}, {{"AX", 2}});
    const SeriesKey ar40 = skey("Ar40", "H1", kSignal);
    const SeriesKey ar39 = skey("Ar39", "AX", kSignal);
    view.on_series(readings(ar40, {{1, 10}}));
    view.on_series(readings(ar39, {{1, 5}}));
    pychron::ui::set_plot_style(changed_style());
    QCOMPARE(view.series_color(ar40).name(), QStringLiteral("#123456"));
    QCOMPARE(panel_pen(view, 0).name(), QStringLiteral("#123456"));
    QCOMPARE(view.series_color(ar39).name(), QColor::fromHsv(50, 200, 150).name());
  }

  void palette_change_keeps_peak_hop_lines_apart() {
    EvolutionsView view({}, {{"H1", 0}});
    const SeriesKey ar40 = skey("Ar40", "H1", kSignal);
    const SeriesKey ar36 = skey("Ar36", "H1", kSignal);
    view.on_series(readings(ar40, {{1, 10}}));
    view.on_series(readings(ar36, {{2, 1}}));
    QVERIFY(view.series_color(ar40) != view.series_color(ar36));
    pychron::ui::set_plot_style(changed_style());
    QVERIFY(view.series_color(ar40) != view.series_color(ar36));
    QCOMPARE(view.series_color(ar40).name(), QColor::fromHsv(0, 200, 150).name());
    QCOMPARE(panel_pen(view, 0), view.series_color(ar40));
    QCOMPARE(panel_pen(view, 1), view.series_color(ar36));
  }

  void onePanelPerSeriesOfTheShownKind() {
    EvolutionsView view;
    view.on_series(readings(skey("Ar40", "H1", kSignal), {{1, 10}}));
    view.on_series(readings(skey("Ar39", "AX", kSignal), {{1, 5}}));
    view.on_series(readings(skey("Ar36", "CDD", kSignal), {{1, 1}}));
    view.on_series(readings(skey("", "H1", kBaseline), {{1, 0.01}}));
    QCOMPARE(view.graph_count(), 3);
    QCOMPARE(view.panel_titles().size(), 3);
    view.set_kind(kBaseline);
    QCOMPARE(view.graph_count(), 1);
    QCOMPARE(view.panel_titles(), QStringList{"H1"});
  }

  void aLabelReadAtTwoPositionsOnOneDetectorIsOnePanel() {
    EvolutionsView view;
    auto first = skey("Ar40", "H1", kSignal);
    auto second = first;
    second.segment = 2;
    second.position = 1;
    view.on_series(readings(first, {{1, 10}}));
    view.on_series(readings(second, {{2, 11}}));
    QCOMPARE(view.graph_count(), 1);
    QCOMPARE(view.panel_titles(), QStringList{"Ar40 H1"});
    QCOMPARE(view.point_count(kSignal), std::size_t{2});
  }

  void aNewSeriesIsAddedAtTheBottom() {
    EvolutionsView view;
    view.on_series(readings(skey("Ar40", "H1", kSignal), {{1, 10}}));
    view.on_series(readings(skey("Ar36", "CDD", kSignal), {{1, 1}}));
    QCOMPARE(view.panel_titles(), (QStringList{"Ar40 H1", "Ar36 CDD"}));
    view.on_series(readings(skey("Ar39", "AX", kSignal), {{2, 5}}));
    QCOMPARE(view.panel_titles(), (QStringList{"Ar40 H1", "Ar36 CDD", "Ar39 AX"}));
  }

  void eachPanelScalesItsOwnY() {
    EvolutionsView view;
    view.on_series(readings(skey("Ar40", "H1", kSignal), {{1, 999}, {2, 1000}, {3, 1001}}));
    view.on_series(readings(skey("Ar36", "CDD", kSignal), {{1, 0.9}, {2, 1.0}, {3, 1.1}}));
    // Waits for a refresh: before one, a panel has QCustomPlot's default range (0 to 5).
    QTRY_VERIFY(holds(view.panel_y_range(0), 999) && holds(view.panel_y_range(0), 1001));
    QVERIFY(holds(view.panel_y_range(1), 0.9) && holds(view.panel_y_range(1), 1.1));
    QVERIFY(view.panel_y_range(1).second < 2);  // its own scale, not Ar40's
    QVERIFY(view.panel_y_range(0).first > 900);
  }

  void everyPanelSharesTheTimeAxis() {
    EvolutionsView view;
    view.on_series(readings(skey("Ar40", "H1", kSignal), {{10, 1}, {20, 2}}));
    view.on_series(readings(skey("Ar36", "CDD", kSignal), {{15, 1}, {30, 2}}));
    QTRY_VERIFY(view.panel_x_range(0).first <= 10 && view.panel_x_range(0).second >= 30);
    QCOMPARE(view.panel_x_range(1), view.panel_x_range(0));
  }

  void onlyTheBottomPanelHasTimeLabels() {
    EvolutionsView view;
    for (const char* iso : {"Ar40", "Ar39", "Ar36"}) view.on_series(readings(skey(iso, "H1", kSignal), {{1, 1}}));
    QCOMPARE(view.graph_count(), 3);
    QVERIFY(!view.panel_has_time_labels(0));
    QVERIFY(!view.panel_has_time_labels(1));
    QVERIFY(view.panel_has_time_labels(2));
  }

  void aRunStartingEmptiesThePanels() {
    EvolutionsView view;
    view.on_series(readings(skey("Ar40", "H1", kSignal), {{1, 10}}));
    view.on_series(readings(skey("Ar36", "CDD", kSignal), {{1, 1}}));
    view.on_run_started({1, "uuid", "66002", {}});
    QCOMPARE(view.graph_count(), 0);
    QVERIFY(view.panel_titles().isEmpty());
    view.on_series(readings(skey("Ar36", "CDD", kSignal), {{1, 1}}));
    QCOMPARE(view.panel_titles(), QStringList{"Ar36 CDD"});
  }

  void aFlatSeriesStillHasARange() {
    EvolutionsView view;
    view.on_series(readings(skey("", "H1", kSignal), {{1, 200.0}, {2, 200.0}, {3, 200.0}}));
    QTRY_VERIFY(holds(view.panel_y_range(0), 200.0));
    QVERIFY(view.panel_y_range(0).second > view.panel_y_range(0).first);
  }

  void aKindWithoutSeriesShowsNoPanels() {
    EvolutionsView view;
    view.on_series(readings(skey("Ar40", "H1", kSignal), {{1, 10}}));
    view.on_series(readings(skey("Ar36", "CDD", kSignal), {{1, 1}}));
    view.set_kind(kSniff);
    QCOMPARE(view.graph_count(), 0);
    QVERIFY(view.panel_titles().isEmpty());
    view.set_kind(kSignal);
    QCOMPARE(view.graph_count(), 2);
  }

  void aReadingThatIsNoNumberLeavesTheRangeFinite() {
    EvolutionsView view;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    view.on_series(readings(skey("Ar40", "H1", kSignal), {{1, 10.0}, {2, nan}, {3, 30.0}}));
    QTRY_VERIFY(holds(view.panel_y_range(0), 10.0) && holds(view.panel_y_range(0), 30.0));
    QVERIFY(std::isfinite(view.panel_y_range(0).first) && std::isfinite(view.panel_y_range(0).second));
  }

  void aSeriesOfAnotherKindLeavesTheShownPanels() {
    EvolutionsView view;
    view.on_series(readings(skey("Ar40", "H1", kSignal), {{1, 10}}));
    view.on_series(readings(skey("Ar36", "CDD", kSignal), {{1, 1}}));
    const QStringList shown = view.panel_titles();
    view.on_series(readings(skey("", "H1", kBaseline), {{5, 0.01}}));
    QCOMPARE(view.panel_titles(), shown);
    view.set_kind(kBaseline);
    QCOMPARE(view.panel_titles(), QStringList{"H1"});
  }

  void aNewSeriesIsScaledAtOnce() {
    EvolutionsView view;
    view.on_series(readings(skey("Ar40", "H1", kSignal), {{1, 999}, {2, 1001}}));
    // No wait for the refresh timer: a panel is never shown at QCustomPlot's default 0 to 5.
    QVERIFY(holds(view.panel_y_range(0), 999) && holds(view.panel_y_range(0), 1001));
    view.on_series(readings(skey("Ar36", "CDD", kSignal), {{1, 100}, {2, 101}}));
    QVERIFY(holds(view.panel_y_range(1), 100) && holds(view.panel_y_range(1), 101));
    QVERIFY(holds(view.panel_y_range(0), 999));
  }

  void theBottomPanelIsAsTallAsTheOthers() {
    EvolutionsView view;
    view.resize(400, 900);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    for (const char* iso : {"Ar40", "Ar39", "Ar38", "Ar37"}) view.on_series(readings(skey(iso, "H1", kSignal), {{1, 1}}));
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
    view.on_series(readings(skey("Ar40", "H1", kSignal), {{1, 10}}));
    view.on_series(readings(skey("Ar36", "CDD", kSignal), {{1, 1}}));
    QCOMPARE(view.graph_count(), 2);
    QTRY_COMPARE(view.scroll_area()->verticalScrollBar()->maximum(), 0);  // two fit
    for (int i = 0; i < 8; ++i)
      view.on_series(readings(skey("Ar" + std::to_string(30 + i), "L" + std::to_string(i), kSignal), {{1, 1}}));
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
    const std::vector<SeriesKey> arrived{skey("Ar40", "H1", kSignal),
                                         skey("Ar36", "CDD", kSignal),
                                         skey("Ar39", "AX", kSignal)};
    const auto panels = evolution_panels(arrived, kSignal);
    QCOMPARE(titles(panels), (QStringList{"Ar40 H1", "Ar36 CDD", "Ar39 AX"}));
    QCOMPARE(panels.size(), std::size_t{3});
    for (std::size_t i = 0; i < panels.size(); ++i) QVERIFY(panels[i].key == arrived[i]);
  }

  void anIsotopeOnTwoDetectorsHasTwoPanels() {
    const auto panels = evolution_panels({skey("Ar40", "H1", kSignal), skey("Ar40", "H2", kSignal)},
                                         kSignal);
    QCOMPARE(titles(panels), (QStringList{"Ar40 H1", "Ar40 H2"}));
  }

  void baselinePanelsAreTitledByDetector() {
    QCOMPARE(titles(evolution_panels({skey("", "H1", kBaseline)}, kBaseline)), QStringList{"H1"});
    QCOMPARE(titles(evolution_panels({skey("Ar40", "H1", kBaseline)}, kBaseline)),
             QStringList{"Ar40 H1"});
  }

  void sniffPanelsAreTitledByIsotope() {
    QCOMPARE(titles(evolution_panels({skey("Ar40", "H1", kSniff)}, kSniff)), QStringList{"Ar40"});
  }

  void eachKindKeepsItsOwnOrder() {
    const std::vector<SeriesKey> arrived{skey("", "H1", kBaseline),
                                         skey("Ar40", "H1", kSignal),
                                         skey("", "AX", kBaseline),
                                         skey("Ar39", "AX", kSignal)};
    QCOMPARE(titles(evolution_panels(arrived, kSignal)), (QStringList{"Ar40 H1", "Ar39 AX"}));
    QCOMPARE(titles(evolution_panels(arrived, kBaseline)), (QStringList{"H1", "AX"}));
    QVERIFY(evolution_panels(arrived, kSniff).empty());
  }

  void hasAPeakCenterTabAfterTheKinds() {
    EvolutionsView view;
    QCOMPARE(tab_bar(view)->count(), 4);
    QCOMPARE(tab_bar(view)->tabText(kPeakCenterTab), QStringLiteral("Peak center"));
  }

  void thePeakCenterTabShowsTheGraphAndHidesTheKindControls() {
    EvolutionsView view;
    QVERIFY(kind_page_shown(view));
    QVERIFY(!view.peak_center_view()->isVisibleTo(&view));
    view.set_kind(kBaseline);
    tab_bar(view)->setCurrentIndex(kPeakCenterTab);
    QVERIFY(view.peak_center_view()->isVisibleTo(&view));
    QVERIFY(!kind_page_shown(view));
    QVERIFY(view.kind() == kBaseline);
    // Back to the kind it was on: the kind is unchanged, its page is shown again.
    tab_bar(view)->setCurrentIndex(1);
    QVERIFY(kind_page_shown(view));
    QVERIFY(!view.peak_center_view()->isVisibleTo(&view));
    QVERIFY(view.kind() == kBaseline);
    tab_bar(view)->setCurrentIndex(kPeakCenterTab);
    tab_bar(view)->setCurrentIndex(0);
    QVERIFY(kind_page_shown(view));
    QVERIFY(view.kind() == kSignal);
  }

  void aTryStartingSelectsTheTab() {
    EvolutionsView view;
    QCOMPARE(tab_bar(view)->currentIndex(), 0);
    view.on_peak_center_try(try_started(0));
    QCOMPARE(tab_bar(view)->currentIndex(), kPeakCenterTab);
    QVERIFY(view.peak_center_view()->isVisibleTo(&view));
    QCOMPARE(view.peak_center_view()->title(), QStringLiteral("Ar40 on H1, try 1 of 2"));
    view.on_peak_center_point({0, sweep_point(5.0, 10)});
    QCOMPARE(view.peak_center_view()->series().front().x.size(), 1);
  }

  void aSeriesArrivingDoesNotLeaveIt() {
    EvolutionsView view;
    view.on_peak_center_try(try_started(0));
    view.on_series(readings(skey("Ar40", "H1", kSignal), {{1, 10}}));
    view.on_series(readings(skey("Ar36", "CDD", kSignal), {{1, 1}}));
    QCOMPARE(tab_bar(view)->currentIndex(), kPeakCenterTab);
    QVERIFY(view.peak_center_view()->isVisibleTo(&view));
    tab_bar(view)->setCurrentIndex(0);
    QCOMPARE(view.graph_count(), 2);
    QCOMPARE(view.panel_titles(), (QStringList{"Ar40 H1", "Ar36 CDD"}));
  }

  void aKindSetInCodeDoesNotLeaveIt() {
    EvolutionsView view;
    view.on_peak_center_try(try_started(0));
    view.set_kind(kBaseline);
    QCOMPARE(tab_bar(view)->currentIndex(), kPeakCenterTab);
    QVERIFY(view.kind() == kBaseline);
  }

  void aPeakCenterStillAddsItsLine() {
    EvolutionsView view;
    view.on_peak_center(done(5.123456));
    QCOMPARE(view.peak_centers(), QStringList{QStringLiteral("Ar40 on H1: center 5.123456")});
    QCOMPARE(view.peak_center_view()->kept(), 1);
    QCOMPARE(view.peak_center_view()->title(), QStringLiteral("Ar40 on H1: center 5.123456"));
  }

  void clickingALineShowsThatPeakCenter() {
    EvolutionsView view;
    view.resize(400, 700);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    view.on_peak_center(done(5.1));
    view.on_peak_center(done(5.2));
    QVERIFY(view.peak_center_view()->title().contains(QStringLiteral("5.200000")));
    QCOMPARE(tab_bar(view)->currentIndex(), 0);
    click_row(peak_list(view), 0);
    QCOMPARE(tab_bar(view)->currentIndex(), kPeakCenterTab);
    QCOMPARE(view.peak_center_view()->title(), QStringLiteral("Ar40 on H1: center 5.100000"));
  }

  void aRunStartingKeepsTheGraphAndItsKeptResults() {
    EvolutionsView view;
    view.on_peak_center(done(5.1));
    view.on_run_started({1, "uuid", "66002", {}});
    QVERIFY(view.peak_centers().isEmpty());  // the list is the run's
    QCOMPARE(view.peak_center_view()->kept(), 1);
    QVERIFY(!view.peak_center_view()->series().empty());
    QCOMPARE(view.peak_center_view()->title(), QStringLiteral("Ar40 on H1: center 5.100000"));
  }

  void aRunStartingAbandonsAnOpenTry() {
    EvolutionsView view;
    view.on_peak_center_try(try_started(0));
    view.on_peak_center_point({0, sweep_point(5.0, 10)});
    view.on_run_started({1, "uuid", "66002", {}});
    QCOMPARE(view.peak_center_view()->title(), QStringLiteral("Ar40 on H1, try 1 of 2: not finished"));
    QCOMPARE(view.peak_center_view()->kept(), 0);
  }

  void aRunStartingReturnsToTheSignalTab() {
    EvolutionsView view;
    view.set_kind(kSniff);
    view.on_peak_center_try(try_started(0));
    view.on_run_started({1, "uuid", "66002", {}});
    QCOMPARE(tab_bar(view)->currentIndex(), 0);
    QVERIFY(view.kind() == kSignal);
    QVERIFY(kind_page_shown(view));
    // From another kind's tab too.
    tab_bar(view)->setCurrentIndex(2);
    view.on_run_started({2, "uuid2", "66003", {}});
    QCOMPARE(tab_bar(view)->currentIndex(), 0);
    QVERIFY(view.kind() == kSignal);
  }

  void clearingPeakCentersEmptiesTheGraph() {
    EvolutionsView view;
    view.on_peak_center(done(5.1));
    view.clear();  // a run's clear leaves the graph
    QCOMPARE(view.peak_center_view()->kept(), 1);
    view.clear_peak_centers();
    QCOMPARE(view.peak_center_view()->kept(), 0);
    QVERIFY(view.peak_center_view()->series().empty());
  }

  void theListOutlivesTheTwentyKept() {
    EvolutionsView view;
    view.resize(400, 700);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    constexpr int kDone = 22;
    const auto center = [](int i) { return 5.0 + 0.001 * i; };
    for (int i = 0; i < kDone; ++i) view.on_peak_center(done(center(i)));
    QCOMPARE(view.peak_centers().size(), kDone);
    QCOMPARE(view.peak_center_view()->kept(), static_cast<int>(pychron::ui::PeakCenterView::kKept));
    const QString last = view.peak_center_view()->title();
    QVERIFY(last.contains(QString::number(center(kDone - 1), 'f', 6)));
    // The two oldest are no longer kept: clicking them changes nothing.
    for (int row : {0, 1}) {
      click_row(peak_list(view), row);
      QCOMPARE(view.peak_center_view()->title(), last);
      QCOMPARE(tab_bar(view)->currentIndex(), 0);
    }
    // The third is the oldest kept.
    click_row(peak_list(view), 2);
    QCOMPARE(tab_bar(view)->currentIndex(), kPeakCenterTab);
    QVERIFY(view.peak_center_view()->title().contains(QString::number(center(2), 'f', 6)));
    click_row(peak_list(view), kDone - 1);
    QCOMPARE(view.peak_center_view()->title(), last);
  }

  void theKeptCountFollowsAQueueAcrossRuns() {
    EvolutionsView view;
    view.resize(400, 700);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    view.on_peak_center(done(5.1));
    view.on_run_started({1, "uuid", "66002", {}});
    view.on_peak_center(done(5.2));
    view.on_peak_center(done(5.3));
    click_row(peak_list(view), 0);  // this run's first: the queue's second
    QCOMPARE(view.peak_center_view()->title(), QStringLiteral("Ar40 on H1: center 5.200000"));
    view.clear_peak_centers();
    view.on_run_started({2, "uuid2", "66003", {}});
    view.on_peak_center(done(5.4));
    click_row(peak_list(view), 0);
    QCOMPARE(view.peak_center_view()->title(), QStringLiteral("Ar40 on H1: center 5.400000"));
  }

  void aFitBeforeItsPointsIsIgnored() {
    EvolutionsView view;
    FitsUpdated fu;
    fu.fits.push_back(SeriesFit{skey("Ar39", "H2", kSignal), reduction::Intercept{}});
    view.on_fits({fu});
    QVERIFY(!view.fit_of(skey("Ar39", "H2", kSignal)).has_value());
  }
};

QTEST_MAIN(TestEvolutionsView)
#include "test_evolutions_view.moc"
