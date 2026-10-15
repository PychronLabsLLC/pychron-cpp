// EvolutionsView fed synthetic series and fits: curves to time zero,
// intercepts, outliers.

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
    QVERIFY(view.plot()->xAxis->range().lower <= 10.0);

    // Other kinds have their own fits; baselines have none here.
    view.set_kind(SeriesKind::Baseline);
    QCOMPARE(view.fit_curve_count(), 0);
    QVERIFY(view.intercept_rows().isEmpty());
    view.set_kind(SeriesKind::Signal);
    QCOMPARE(view.fit_curve_count(), 1);
    QCOMPARE(view.intercept_rows().size(), 1);

    // One series alone, scaled to it.
    const SeriesKey ar39{"Ar39", "H2", SeriesKind::Signal};
    view.on_series({point(ar39, 11, 5.0), point(ar39, 12, 5.1)});
    QCOMPARE(view.graph_count(), 2);
    QCOMPARE(view.focus_choices(), (QStringList{QStringLiteral("All series"), QStringLiteral("Ar39 H2"), QStringLiteral("Ar40 H1")}));
    view.set_focus(ar39);
    QCOMPARE(view.graph_count(), 1);
    QTRY_VERIFY(view.plot()->yAxis->range().upper < 10);
    view.set_focus(std::nullopt);
    QCOMPARE(view.graph_count(), 2);

    // A new run clears the fits.
    view.on_run_started({1, "uuid2", "66002", {}});
    QVERIFY(!view.fit_of(ar40).has_value());
    QCOMPARE(view.fit_curve_count(), 0);
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
