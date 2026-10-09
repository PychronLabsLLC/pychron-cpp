// SceneView draws what a scene says; here, the layers with nothing else to test them.

#include <cmath>
#include <memory>

#include <QtTest/QtTest>

#include <qcustomplot.h>

#include "pychron/processing/scene.hpp"
#include "scene_view.hpp"

namespace pp = pychron::processing;
using pychron::ui::SceneView;

class TestSceneView : public QObject {
  Q_OBJECT

 private slots:
  // A span with x bounds alone runs the whole height of its panel; one with
  // every bound is that rectangle. Neither changes the axes.
  void spansFillTheirBoundsOrThePanel() {
    pp::Scene scene;
    pp::Graph g;
    g.x.min = 0.0;
    g.x.max = 10.0;
    pp::Panel p;
    p.y.min = 0.0;
    p.y.max = 100.0;
    pp::SpanLayer tall;
    tall.x0 = 2.0;
    tall.x1 = 4.0;
    tall.label = "FC";
    pp::SpanLayer box;
    box.x0 = 5.0;
    box.x1 = 10.0;
    box.y0 = 25.0;
    box.y1 = 75.0;
    pp::SpanLayer open;  // no bounds: the panel
    pp::PointLayer pts;
    pts.x = {1.0, 9.0};
    pts.y = {10.0, 90.0};
    pts.refs = {pp::PointRef{"a"}, pp::PointRef{"b"}};
    pts.excluded = {false, false};
    p.layers = {tall, box, open, pts};
    g.panels.push_back(p);
    scene.graphs.push_back(g);

    SceneView view;
    view.resize(600, 400);
    view.set_scene(std::make_shared<const pp::Scene>(scene));
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    view.plot()->replot();

    const QCPAxisRect* rect = view.plot()->axisRect(0);
    const QRectF panel = rect->rect();
    const QCPAxis* x = rect->axis(QCPAxis::atBottom);
    const QCPAxis* y = rect->axis(QCPAxis::atLeft);
    QCOMPARE(x->range().lower, 0.0);
    QCOMPARE(x->range().upper, 10.0);

    const QList<QRectF> spans = view.span_rects(0);
    QCOMPARE(spans.size(), 3);
    const auto near = [](double a, double b) { return qAbs(a - b) < 1.5; };
    QVERIFY(near(spans[0].left(), x->coordToPixel(2.0)));
    QVERIFY(near(spans[0].right(), x->coordToPixel(4.0)));
    QVERIFY(near(spans[0].top(), panel.top()));
    QVERIFY(near(spans[0].bottom(), panel.bottom()));
    QVERIFY(near(spans[1].left(), x->coordToPixel(5.0)));
    QVERIFY(near(spans[1].top(), y->coordToPixel(75.0)));
    QVERIFY(near(spans[1].bottom(), y->coordToPixel(25.0)));
    QVERIFY(near(spans[2].left(), panel.left()));
    QVERIFY(near(spans[2].right(), panel.right()));
    QVERIFY(spans[1].height() < spans[0].height());
  }

  // A click acts on an analysis: a point that names none (a mean, a predicted
  // value) drawn over one does not hide it, and alone is no target. Its tooltip
  // is still the one shown there.
  void aClickReachesTheAnalysisUnderAPointWithoutOne() {
    pp::Scene scene;
    pp::Graph g;
    g.x.min = 0.0;
    g.x.max = 10.0;
    pp::Panel p;
    p.y.min = 0.0;
    p.y.max = 100.0;
    pp::PointLayer analyses;
    analyses.x = {3.0, 7.0};
    analyses.y = {30.0, 70.0};
    analyses.refs = {pp::PointRef{"a"}, pp::PointRef{"b"}};
    analyses.excluded = {false, false};
    analyses.tooltips = {"analysis a", "analysis b"};
    pp::PointLayer means;  // drawn after, so on top: one exactly over "a", one alone
    means.x = {3.0, 5.0};
    means.y = {30.0, 50.0};
    means.refs = {pp::PointRef{""}, pp::PointRef{""}};
    means.excluded = {false, false};
    means.tooltips = {"mean over a", "mean alone"};
    p.layers = {analyses, means};
    g.panels.push_back(p);
    scene.graphs.push_back(g);

    SceneView view;
    view.resize(600, 400);
    view.set_scene(std::make_shared<const pp::Scene>(scene));
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    view.plot()->replot();

    QStringList clicked;
    connect(&view, &SceneView::point_clicked, &view, [&](const QString& id) { clicked << id; });
    QStringList banded;
    connect(&view, &SceneView::points_toggled, &view, [&](const QStringList& ids) { banded = ids; });
    const QCPAxisRect* rect = view.plot()->axisRect(0);
    const auto at = [&](double x, double y) {
      return QPoint(static_cast<int>(std::lround(rect->axis(QCPAxis::atBottom)->coordToPixel(x))),
                    static_cast<int>(std::lround(rect->axis(QCPAxis::atLeft)->coordToPixel(y))));
    };
    const auto pos = view.point_position("a");
    QVERIFY(pos);
    QCOMPARE(view.plot()->mapFrom(&view, *pos), at(3.0, 30.0));

    // Under the mean: the analysis is clicked, and the tooltip is the mean's.
    QCOMPARE(view.tooltip_at(at(3.0, 30.0)), QStringLiteral("mean over a"));
    QTest::mouseClick(view.plot(), Qt::LeftButton, Qt::NoModifier, at(3.0, 30.0));
    QCOMPARE(clicked, QStringList{QStringLiteral("a")});

    // A point that names no analysis, alone: a tooltip, and nothing to click.
    QCOMPARE(view.tooltip_at(at(5.0, 50.0)), QStringLiteral("mean alone"));
    QTest::mouseClick(view.plot(), Qt::LeftButton, Qt::NoModifier, at(5.0, 50.0));
    QCOMPARE(clicked.size(), 1);

    // An analysis with nothing over it, as ever.
    QTest::mouseClick(view.plot(), Qt::LeftButton, Qt::NoModifier, at(7.0, 70.0));
    QCOMPARE(clicked, QStringList({QStringLiteral("a"), QStringLiteral("b")}));

    // A rubber band over all of it names the analyses only, each once.
    QTest::mousePress(view.plot(), Qt::LeftButton, Qt::ShiftModifier, at(1.0, 90.0));
    QTest::mouseMove(view.plot(), at(9.0, 10.0));
    QTest::mouseRelease(view.plot(), Qt::LeftButton, Qt::ShiftModifier, at(9.0, 10.0));
    QCOMPARE(banded, QStringList({QStringLiteral("a"), QStringLiteral("b")}));
    // And over the lone mean, nothing.
    banded.clear();
    QTest::mousePress(view.plot(), Qt::LeftButton, Qt::ShiftModifier, at(4.5, 55.0));
    QTest::mouseMove(view.plot(), at(5.5, 45.0));
    QTest::mouseRelease(view.plot(), Qt::LeftButton, Qt::ShiftModifier, at(5.5, 45.0));
    QVERIFY(banded.isEmpty());
  }

  // Stacked panels are exactly `panel_spacing` apart, zero included: nothing
  // of a panel's own lies between it and the next. Graphs are `graph_spacing`
  // apart.
  void spacingIsWhatTheSceneSays() {
    const auto scene_with = [](int panel_spacing, int graph_spacing) {
      pp::Scene scene;
      scene.columns = 2;
      scene.style.panel_spacing = panel_spacing;
      scene.style.graph_spacing = graph_spacing;
      for (int gi = 0; gi < 2; ++gi) {
        pp::Graph g;
        g.x.title = "Age (Ma)";
        for (int pi = 0; pi < 3; ++pi) {
          pp::Panel p;
          p.y.title = "y";
          pp::PointLayer pts;
          pts.x = {1.0, 9.0};
          pts.y = {10.0, 90.0};
          pts.refs = {pp::PointRef{"a"}, pp::PointRef{"b"}};
          pts.excluded = {false, false};
          p.layers = {pts};
          g.panels.push_back(p);
        }
        scene.graphs.push_back(g);
      }
      return std::make_shared<const pp::Scene>(scene);
    };
    SceneView view;
    view.resize(800, 600);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    const auto panel_gap = [&](int upper) {
      const QRect a = view.plot()->axisRect(upper)->rect(), b = view.plot()->axisRect(upper + 1)->rect();
      return b.y() - (a.y() + a.height());
    };
    const auto graph_gap = [&] {
      const QRect a = view.plot()->plotLayout()->elementAt(0)->outerRect();
      const QRect b = view.plot()->plotLayout()->elementAt(1)->outerRect();
      return b.x() - (a.x() + a.width());
    };
    for (const int spacing : {0, 10}) {
      view.set_scene(scene_with(spacing, 5));
      view.plot()->replot();
      QCOMPARE(panel_gap(0), spacing);
      QCOMPARE(panel_gap(1), spacing);
      QCOMPARE(panel_gap(3), spacing);  // the second graph
      QCOMPARE(graph_gap(), 5);
    }
    for (const int spacing : {0, 30}) {
      view.set_scene(scene_with(4, spacing));
      view.plot()->replot();
      QCOMPARE(graph_gap(), spacing);
    }
  }
};

QTEST_MAIN(TestSceneView)
#include "test_scene_view.moc"
