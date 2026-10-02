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
    QVERIFY(view.intercept_lines().isEmpty());

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
    QCOMPARE(view.intercept_lines().size(), 1);
    QVERIFY2(view.intercept_lines().front().startsWith(QStringLiteral("Ar40 H1  linear  100 ± ")),
             qPrintable(view.intercept_lines().front()));

    // The x axis reaches back to time zero, where the intercept is.
    QVERIFY(view.plot()->xAxis->range().lower <= 10.0);

    // Other kinds have their own fits; baselines have none here.
    view.set_kind(SeriesKind::Baseline);
    QCOMPARE(view.fit_curve_count(), 0);
    QVERIFY(view.intercept_lines().isEmpty());
    view.set_kind(SeriesKind::Signal);
    QCOMPARE(view.fit_curve_count(), 1);

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
