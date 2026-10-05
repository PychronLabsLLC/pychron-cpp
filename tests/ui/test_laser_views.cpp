// TrayView and CameraView (laser window design, section 6).

#include <cmath>

#include <QSignalSpy>
#include <QtTest/QtTest>

#include "camera_view.hpp"
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

class LaserViewsTest : public QObject {
  Q_OBJECT

 private slots:
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
    // the tray's centre at stage (25, 25), a quarter turn: stage +x is the tray's... wherever the inverse says
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
    // the aim crosshair runs through the centre
    QVERIFY(close(shot.pixelColor(120, 200), theme().error));
    QVERIFY(close(shot.pixelColor(200, 320), theme().error));
    // the target's ring: centre (70, 30) radius 15 px, 4 widget px to the frame px
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
};

QTEST_MAIN(LaserViewsTest)
#include "test_laser_views.moc"
