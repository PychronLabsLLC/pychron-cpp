// TrayView and CameraView (laser window design, section 6).

#include <cmath>

#include <QSignalSpy>
#include <QtTest/QtTest>

#include <qcustomplot.h>

#include "camera_view.hpp"
#include "follow_plot_model.hpp"
#include "follow_plot_view.hpp"
#include "plot_style.hpp"
#include "theme.hpp"
#include "tray_view.hpp"

using namespace pychron;
using namespace pychron::ui;

namespace {

// 3 x 3, 5 mm apart, 2 mm holes: 1 top left ... 9 bottom right.
laser::TrayMap nine() {
  auto map = laser::TrayMap::parse(
      "circle,2.0\n\n2,6,8,4,5\n1,-5,5\n2,0,5\n3,5,5\n4,-5,0\n5,0,0\n6,5,0\n7,-5,-5\n8,0,-5\n9,5,-5\n", "nine");
  if (!map) qFatal("%s", map.error().what.c_str());
  return *map;
}

bool close(const QColor& a, const QColor& b) {
  return std::abs(a.red() - b.red()) < 12 && std::abs(a.green() - b.green()) < 12 && std::abs(a.blue() - b.blue()) < 12;
}

laser::CameraView seen(bool with_target) {
  laser::CameraView v;
  v.frame = vision::Frame::make(100, 100, 255, 200);
  v.aim_px = {49.5, 49.5};
  v.px_per_mm = 20;
  v.expected_radius_px = 20;
  if (with_target) {
    vision::Target t;
    t.center_px = {70, 30};
    t.radius_px = 15;
    v.target = t;
  }
  return v;
}

}  // namespace

namespace {

// A followed step: a grain at (3,4), a blob seen, `with_grain` off for a real laser.
pychron::laser::FollowSample follow_sample(double t, bool with_grain = true) {
  pychron::laser::FollowSample s;
  s.step.stage_mm = {1 + t, 2};
  s.step.elapsed_s = t;
  s.step.saturation = 0.5;
  s.step.reason = pychron::vision::DragonflyStep::Reason::Track;
  s.step.seen = pychron::vision::DragonflySeen{{0.3, 0.4}, 3.0};
  if (with_grain) {
    pychron::laser::GrainView g;
    g.grain_mm = {3, 4};
    g.beam_mm = {0, 0};
    g.truth.brightness = 0.7;
    g.truth.radius_mm = 0.5;
    s.grain = g;
  }
  return s;
}

int legend_entries(const pychron::ui::FollowPlotView& view, int strip) {
  auto* legend = qobject_cast<QCPLegend*>(view.plot()->axisRect(strip)->insetLayout()->elementAt(0));
  return legend != nullptr ? legend->itemCount() : -1;
}

}  // namespace

class LaserViewsTest : public QObject {
  Q_OBJECT

 private slots:
  void the_grain_is_a_cross_where_it_really_is() {
    CameraView view;
    view.resize(400, 400);
    laser::CameraView with = seen(false);
    with.grain_px = laser::StageXY{20, 70};
    view.set_view(with);
    QVERIFY(view.has_grain());
    const QImage drawn = view.grab().toImage();
    const QPointF at = view.to_widget({20, 70});
    QVERIFY(close(drawn.pixelColor(at.toPoint()), theme().warning));
    QVERIFY(close(drawn.pixelColor((at + QPointF(5, 0)).toPoint()), theme().warning));
    QVERIFY(close(drawn.pixelColor((at + QPointF(0, -5)).toPoint()), theme().warning));
    QVERIFY(!close(drawn.pixelColor((at + QPointF(5, 5)).toPoint()), theme().warning));

    view.set_view(seen(false));
    QVERIFY(!view.has_grain());
    view.set_view(with);
    view.set_failed(QStringLiteral("gone"));
    QVERIFY(!view.has_grain());
  }

  void a_grain_off_the_picture_is_not_drawn_beside_it() {
    CameraView view;
    view.resize(400, 200);  // the frame is a square in the middle
    laser::CameraView with = seen(false);
    with.grain_px = laser::StageXY{-10, 50};
    view.set_view(with);
    const QImage drawn = view.grab().toImage();
    QVERIFY(!close(drawn.pixelColor(view.to_widget({-10, 50}).toPoint()), theme().warning));
  }

  void tray_maps_holes_to_scale() {
    const auto map = nine();
    TrayView view;
    view.resize(428, 428);  // 400 px for 12 mm of tray, less the padding
    view.set_tray(&map);
    QCOMPARE(view.tray(), QStringLiteral("nine"));
    const QPointF c5 = view.hole_center(QStringLiteral("5"));
    const QPointF c6 = view.hole_center(QStringLiteral("6"));
    const QPointF c2 = view.hole_center(QStringLiteral("2"));
    QCOMPARE(c5, QPointF(214, 214));
    const double per_mm = (c6.x() - c5.x()) / 5.0;
    QVERIFY(std::abs(per_mm - 400.0 / 12.0) < 1e-6);
    QCOMPARE(c6.y(), c5.y());
    QVERIFY(std::abs((c5.y() - c2.y()) - 5 * per_mm) < 1e-6);  // +y is up
    QVERIFY(std::abs(view.hole_radius(QStringLiteral("5")) - per_mm) < 1e-6);
    QVERIFY(view.hole_center(QStringLiteral("nope")).isNull());
  }

  void clicking_a_hole_says_which() {
    const auto map = nine();
    TrayView view;
    view.resize(428, 428);
    view.set_tray(&map);
    QSignalSpy clicked(&view, &TrayView::holeClicked);
    QTest::mouseClick(&view, Qt::LeftButton, {}, view.hole_center(QStringLiteral("3")).toPoint());
    QCOMPARE(clicked.size(), 1);
    QCOMPARE(clicked[0][0].toString(), QStringLiteral("3"));
    // near its edge is still the hole
    const QPoint edge = (view.hole_center(QStringLiteral("7")) + QPointF(view.hole_radius(QStringLiteral("7")) - 2, 0)).toPoint();
    QCOMPARE(view.hole_at(edge), QStringLiteral("7"));
  }

  void clicking_between_holes_says_nothing() {
    const auto map = nine();
    TrayView view;
    view.resize(428, 428);
    view.set_tray(&map);
    QSignalSpy clicked(&view, &TrayView::holeClicked);
    const QPointF between = (view.hole_center(QStringLiteral("5")) + view.hole_center(QStringLiteral("6"))) / 2;
    QTest::mouseClick(&view, Qt::LeftButton, {}, between.toPoint());
    QCOMPARE(clicked.size(), 0);
    QVERIFY(view.hole_at(between).isEmpty());
  }

  void the_stage_is_drawn_only_with_a_calibration() {
    const auto map = nine();
    TrayView view;
    view.resize(428, 428);
    view.set_tray(&map);
    view.set_stage(laser::StageXY{30, 25});
    QVERIFY(!view.stage_point());
    // the tray's center at stage (25, 25), a quarter turn: stage +x is the tray's... wherever the inverse says
    laser::Transform t;
    t.cx = 25;
    t.cy = 25;
    view.set_transform(t);
    QVERIFY(view.stage_point().has_value());
    QCOMPARE(*view.stage_point(), view.hole_center(QStringLiteral("6")));  // stage (30, 25) is hole 6
    t.rotation = std::acos(-1.0) / 2;  // a quarter turn counter-clockwise: tray +x is stage +y
    view.set_transform(t);
    const QPointF at = *view.stage_point();
    const QPointF c8 = view.hole_center(QStringLiteral("8"));  // tray (0, -5) -> stage (30, 25)
    QVERIFY2(std::hypot(at.x() - c8.x(), at.y() - c8.y()) < 1e-6, "stage (30, 25) is hole 8 on a tray a quarter turn round");
    // and it is painted there
    const QImage shot = view.grab().toImage();
    QVERIFY(close(shot.pixelColor(QPoint(int(at.x()) + 7, int(at.y()))), theme().error));
    view.set_stage(std::nullopt);
    QVERIFY(!view.stage_point());
  }

  void marks_show_on_the_holes() {
    const auto map = nine();
    TrayView view;
    view.resize(428, 428);
    view.set_tray(&map);
    view.set_current_hole(QStringLiteral("5"));
    view.set_calibration_holes({QStringLiteral("6")});
    const QImage shot = view.grab().toImage();
    QVERIFY(close(shot.pixelColor(view.hole_center(QStringLiteral("5")).toPoint() + QPoint(10, 10)), theme().accent_soft));
    QVERIFY(close(shot.pixelColor(view.hole_center(QStringLiteral("1")).toPoint() + QPoint(10, 10)), theme().window));
    // the ring is just outside hole 6
    const QPoint ring = (view.hole_center(QStringLiteral("6")) + QPointF(view.hole_radius(QStringLiteral("6")) + 3, 0)).toPoint();
    QVERIFY(close(shot.pixelColor(ring), theme().accent));
  }

  void no_tray_says_so() {
    TrayView view;
    view.resize(300, 300);
    view.set_tray(nullptr);
    QVERIFY(view.tray().isEmpty());
    QVERIFY(view.hole_at(QPointF(150, 150)).isEmpty());
    view.grab();  // paints without a tray
  }

  void camera_shows_frame_aim_and_target() {
    CameraView view;
    view.resize(400, 400);
    view.set_view(seen(true));
    QCOMPARE(view.image().size(), QSize(100, 100));
    QCOMPARE(view.image().pixelColor(10, 10), QColor(200, 200, 200));
    QVERIFY(view.has_target());
    QVERIFY(view.message().isEmpty());
    QCOMPARE(view.to_widget(QPointF(49.5, 49.5)), QPointF(200, 200));
    const QImage shot = view.grab().toImage();
    // the picture
    QVERIFY(close(shot.pixelColor(20, 20), QColor(200, 200, 200)));
    // the aim crosshair runs through the center
    QVERIFY(close(shot.pixelColor(120, 200), theme().error));
    QVERIFY(close(shot.pixelColor(200, 320), theme().error));
    // the target's ring: center (70, 30) radius 15 px, 4 widget px to the frame px
    const QPointF c = view.to_widget(QPointF(70, 30));
    QVERIFY(close(shot.pixelColor(QPoint(int(c.x() + 60), int(c.y()))), theme().ok));
    QVERIFY(close(shot.pixelColor(QPoint(int(c.x()), int(c.y()))), QColor(200, 200, 200)));
  }

  void camera_without_a_target_draws_none() {
    CameraView view;
    view.resize(400, 400);
    view.set_view(seen(true));
    view.set_view(seen(false));
    QVERIFY(!view.has_target());
    const QImage shot = view.grab().toImage();
    const QPointF c = view.to_widget(QPointF(70, 30));
    QVERIFY(close(shot.pixelColor(QPoint(int(c.x() + 60), int(c.y()))), QColor(200, 200, 200)));
  }

  void a_failed_camera_keeps_its_picture_and_says_why() {
    CameraView view;
    view.resize(400, 400);
    view.set_view(seen(true));
    view.set_failed(QStringLiteral("camera: no frame"));
    QVERIFY(!view.image().isNull());
    QCOMPARE(view.message(), QStringLiteral("camera: no frame"));
    QVERIFY(!view.has_target());
    const QImage shot = view.grab().toImage();
    QVERIFY(!close(shot.pixelColor(20, 20), QColor(200, 200, 200)));  // greyed
  }

  // A live camera that has stopped: its last picture, greyed, how old, and why.
  void a_stopped_live_camera_shows_its_last_picture_and_why() {
    CameraView view;
    view.resize(400, 400);
    laser::CameraView last = seen(true);
    last.trouble = "cable out";
    last.age_ms = 4200;
    view.set_view(last);
    QVERIFY(!view.image().isNull());
    QVERIFY2(view.message().contains(QStringLiteral("cable out")), qPrintable(view.message()));
    QVERIFY2(view.message().contains(QStringLiteral("4 s")), qPrintable(view.message()));
    QVERIFY(!view.has_target());
    const QImage shot = view.grab().toImage();
    QVERIFY(!close(shot.pixelColor(20, 20), QColor(200, 200, 200)));  // greyed
    // and it clears when frames flow again
    view.set_view(seen(true));
    QVERIFY(view.message().isEmpty());
    QVERIFY(view.has_target());
  }

  void camera_says_when_it_has_none() {
    CameraView view;
    view.resize(300, 300);
    view.clear(QStringLiteral("co2 has no camera"));
    QVERIFY(view.image().isNull());
    QCOMPARE(view.message(), QStringLiteral("co2 has no camera"));
    view.grab();
  }

  void follow_plot_has_five_strips() {
    pychron::ui::FollowPlotModel model;
    pychron::ui::FollowPlotView view(model);
    QCOMPARE(view.strip_count(), 5);
    QCOMPARE(view.plot()->axisRectCount(), 5);
    const QStringList titles = {QStringLiteral("Stage X (mm)"), QStringLiteral("Stage Y (mm)"),
                                QStringLiteral("Beam deviation (mm)"), QStringLiteral("Luminosity"),
                                QStringLiteral("Grain size (mm)")};
    for (int i = 0; i < 5; ++i) QCOMPARE(view.plot()->axisRect(i)->axis(QCPAxis::atLeft)->label(), titles[i]);
    QCOMPARE(view.windowTitle(), QStringLiteral("Follow plots"));
    view.grab();  // empty model: nothing non-finite handed to the plot
  }

  void follow_plot_draws_the_model() {
    pychron::ui::FollowPlotModel model;
    pychron::ui::FollowPlotView view(model);
    for (int i = 0; i < 3; ++i) model.append(follow_sample(i * 0.5));
    view.refresh();
    QCOMPARE(view.point_count(pychron::ui::FollowLine::StageX), 3);
    QCOMPARE(view.point_count(pychron::ui::FollowLine::GrainX), 3);
    QVERIFY(view.line_visible(pychron::ui::FollowLine::SizeTrue));
    QCOMPARE(legend_entries(view, 0), 2);
    QCOMPARE(legend_entries(view, 4), 2);
    view.grab();
    view.resize(400, 500);
    model.clear();
    model.append(follow_sample(0));  // one row: a range of nothing
    view.refresh();
    const QCPRange x = view.plot()->axisRect(0)->axis(QCPAxis::atBottom)->range();
    QVERIFY(std::isfinite(x.lower) && std::isfinite(x.upper) && x.upper > x.lower);
    for (int i = 0; i < 5; ++i) {
      const QCPRange y = view.plot()->axisRect(i)->axis(QCPAxis::atLeft)->range();
      QVERIFY(std::isfinite(y.lower) && std::isfinite(y.upper) && y.upper > y.lower);
    }
  }

  void follow_plot_hides_lines_with_no_values() {
    pychron::ui::FollowPlotModel model;
    pychron::ui::FollowPlotView view(model);
    for (int i = 0; i < 3; ++i) model.append(follow_sample(i * 0.5, false));
    view.refresh();
    QVERIFY(view.line_visible(pychron::ui::FollowLine::StageX));
    QVERIFY(view.line_visible(pychron::ui::FollowLine::DeviationSeen));
    QVERIFY(!view.line_visible(pychron::ui::FollowLine::GrainX));
    QVERIFY(!view.line_visible(pychron::ui::FollowLine::SizeTrue));
    QCOMPARE(legend_entries(view, 0), 1);
    QCOMPARE(legend_entries(view, 2), 1);
    view.grab();
  }

  void follow_plot_strips_share_time() {
    pychron::ui::FollowPlotModel model;
    pychron::ui::FollowPlotView view(model);
    for (int i = 0; i < 20; ++i) model.append(follow_sample(i));
    view.refresh();
    view.plot()->axisRect(0)->axis(QCPAxis::atBottom)->setRange(4, 9);
    for (int i = 0; i < 5; ++i) {
      const QCPRange r = view.plot()->axisRect(i)->axis(QCPAxis::atBottom)->range();
      QCOMPARE(r.lower, 4.0);
      QCOMPARE(r.upper, 9.0);
    }
    view.refresh();  // a zoom stays through new rows
    QCOMPARE(view.plot()->axisRect(3)->axis(QCPAxis::atBottom)->range().lower, 4.0);
  }

  void follow_plot_follows_the_palette() {
    pychron::ui::FollowPlotModel model;
    pychron::ui::FollowPlotView view(model);
    model.append(follow_sample(0));
    view.refresh();
    using pychron::ui::plot_series_color;
    QCOMPARE(view.plot()->graph(0)->pen().color(), plot_series_color(0));
    QCOMPARE(view.plot()->graph(1)->pen().color(), plot_series_color(1));
    QCOMPARE(view.plot()->graph(8)->pen().color(), plot_series_color(0));
    QCOMPARE(view.plot()->graph(9)->pen().color(), plot_series_color(1));

    const pychron::ui::PlotStyle before = pychron::ui::plot_style();  // NOLINT(performance-unnecessary-copy-initialization): a copy; set_plot_style replaces the original
    pychron::ui::PlotStyle changed = before;
    changed.series[0] = QColor(QStringLiteral("#123456"));
    changed.series[1] = QColor(QStringLiteral("#654321"));
    changed.plot_background = QColor(QStringLiteral("#eeeeee"));
    pychron::ui::set_plot_style(changed);
    QCOMPARE(view.plot()->graph(0)->pen().color(), QColor(QStringLiteral("#123456")));
    QCOMPARE(view.plot()->graph(3)->pen().color(), QColor(QStringLiteral("#654321")));
    QCOMPARE(view.plot()->axisRect(2)->backgroundBrush().color(), QColor(QStringLiteral("#eeeeee")));
    pychron::ui::set_plot_style(before);
  }
};

QTEST_MAIN(LaserViewsTest)
#include "test_laser_views.moc"
