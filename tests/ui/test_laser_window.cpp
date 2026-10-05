// LaserWindow on the example lab's simulated laser (laser window design,
// sections 1 and 6).

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QtTest/QtTest>

#include "camera_view.hpp"
#include "laser_fixture.hpp"
#include "laser_window.hpp"
#include "main_window.hpp"
#include "pattern_maker_window.hpp"
#include "tray_view.hpp"

using namespace pychron;
using namespace pychron::ui;
using pychron::experiment::lab::Lasers;

class LaserWindowTest : public QObject {
  Q_OBJECT

  template <class W>
  W* the(const char* name) {
    W* w = window_->findChild<W*>(QString::fromLatin1(name));
    if (w == nullptr) qFatal("no widget named %s", name);
    return w;
  }
  QPushButton* btn(const char* name) { return the<QPushButton>(name); }
  QString text(const char* name) { return the<QLabel>(name)->text(); }
  // The bridge's commands have run and the window has drawn what followed.
  void settle() {
    test::settle(*bridge_);
    QCoreApplication::processEvents();
  }
  void choose_tray(const QString& name) {
    auto* combo = the<QComboBox>("tray_choice");
    const int index = combo->findData(name);
    QVERIFY(index >= 0);
    combo->setCurrentIndex(index);
    emit combo->activated(index);
    settle();
  }

 private slots:
  void init() { build(50); }
  void build(double speed) {
    cleanup();
    lab_ = std::make_unique<test::SimLaserLab>(speed);
    bridge_ = std::make_unique<LaserBridge>(lab_->deps());
    window_ = std::make_unique<LaserWindow>(*bridge_, true);
    window_->resize(1000, 700);
    window_->show();
    QVERIFY(QTest::qWaitForWindowExposed(window_.get()));
  }
  void cleanup() {
    window_.reset();
    bridge_.reset();
    lab_.reset();
  }

  void says_what_it_is() {
    QCOMPARE(window_->windowTitle(), QStringLiteral("Laser co2 (Simulation)"));
    QVERIFY(the<QComboBox>("tray_choice")->findData(QStringLiteral("example-9")) >= 0);
    QVERIFY(text("position").contains(QStringLiteral("x 0.000")));
    QVERIFY(!the<QLabel>("banner")->isVisible());
    QVERIFY(!btn("reset_stop")->isVisible());
    QVERIFY(btn("estop")->isEnabled());
  }

  void fire_is_live_only_when_enabled() {
    QVERIFY(!btn("fire")->isEnabled());
    btn("enable")->click();
    settle();
    QTRY_VERIFY(btn("fire")->isEnabled());
    QVERIFY(btn("enable")->isChecked());
    the<QDoubleSpinBox>("output")->setValue(18);
    btn("fire")->click();
    settle();
    QVERIFY(lab_->sim().firing());
    QCOMPARE(lab_->sim().output(), 18.0);
    QTRY_VERIFY2(text("beam").contains(QStringLiteral("FIRING at 18.0")), qPrintable(text("beam")));
    btn("stop_beam")->click();
    settle();
    QVERIFY(!lab_->sim().firing());
    QTRY_VERIFY(text("beam").startsWith(QStringLiteral("beam off")));
    btn("enable")->click();
    settle();
    QVERIFY(!lab_->sim().enabled());
    QTRY_VERIFY(!btn("fire")->isEnabled());
  }

  void an_interlock_disables_fire_and_is_named() {
    btn("enable")->click();
    settle();
    QTRY_VERIFY(btn("fire")->isEnabled());
    lab_->sim().trip_interlock("Door");
    QTRY_VERIFY(!btn("fire")->isEnabled());
    QVERIFY2(text("interlocks").contains(QStringLiteral("Door")), qPrintable(text("interlocks")));
    lab_->sim().clear_interlocks();
    QTRY_VERIFY(btn("fire")->isEnabled());
  }

  void choosing_a_tray_draws_it_with_its_calibration() {
    choose_tray(QStringLiteral("example-9"));
    QCOMPARE(lab_->system().tray(), std::string("example-9"));
    QTRY_COMPARE(window_->tray_view()->tray(), QStringLiteral("example-9"));
    QVERIFY(!window_->tray_view()->hole_center(QStringLiteral("9")).isNull());
    QTRY_VERIFY(window_->tray_view()->stage_point().has_value());
    QCOMPARE(the<QTableWidget>("cal_table")->rowCount(), 2);
    QVERIFY2(text("cal_solution").contains(QStringLiteral("Centre 25.000, 25.000")), qPrintable(text("cal_solution")));
    // two points on one line: a mirrored axis would fit as well, and it says so
    QVERIFY2(!text("cal_cautions").isEmpty(), "two points cannot rule out a mirror");
  }

  void clicking_a_hole_moves_the_stage() {
    choose_tray(QStringLiteral("example-9"));
    the<QCheckBox>("centre_on_go")->setChecked(false);
    TrayView* tray = window_->tray_view();
    QTest::mouseClick(tray, Qt::LeftButton, {}, tray->hole_center(QStringLiteral("3")).toPoint());
    settle();
    QCOMPARE(lab_->x(), 30.0);
    QCOMPARE(lab_->y(), 30.0);
    QTRY_VERIFY2(text("position").contains(QStringLiteral("x 30.000   y 30.000")), qPrintable(text("position")));
    // the crosshair is on the hole it went to
    QTRY_VERIFY(tray->stage_point().has_value());
    const QPointF at = *tray->stage_point(), hole = tray->hole_center(QStringLiteral("3"));
    QVERIFY(std::hypot(at.x() - hole.x(), at.y() - hole.y()) < 0.5);
    QVERIFY2(text("status").contains(QStringLiteral("go to: done")), qPrintable(text("status")));
  }

  void a_click_centres_the_hole_when_asked_and_autocenter_does_it_again() {
    choose_tray(QStringLiteral("example-9"));
    QVERIFY(the<QCheckBox>("centre_on_go")->isChecked());  // the lab gives it a camera
    QVERIFY(!btn("autocenter")->isEnabled());              // the stage is on no hole yet
    TrayView* tray = window_->tray_view();
    QTest::mouseClick(tray, Qt::LeftButton, {}, tray->hole_center(QStringLiteral("7")).toPoint());
    settle();
    QVERIFY(std::abs(lab_->x() - 20.15) < 0.04);
    QVERIFY(std::abs(lab_->y() - 19.90) < 0.04);
    QTRY_VERIFY2(text("autocenter_outcome").contains(QStringLiteral("hole 7 centred")), qPrintable(text("autocenter_outcome")));
    QTRY_VERIFY(btn("autocenter")->isEnabled());
    btn("autocenter")->click();
    settle();
    QVERIFY(std::abs(lab_->x() - 20.15) < 0.04);
    QVERIFY(window_->camera_view()->message().isEmpty());
    QTRY_VERIFY(!window_->camera_view()->image().isNull());
  }

  void jog_buttons_move_by_the_step() {
    the<QDoubleSpinBox>("jog_step")->setValue(2.5);
    btn("jog_right")->click();
    btn("jog_right")->click();
    btn("jog_up")->click();
    btn("jog_z_up")->click();
    settle();
    QCOMPARE(lab_->x(), 5.0);
    QCOMPARE(lab_->y(), 2.5);
    QCOMPARE(lab_->z(), 2.5);
    btn("jog_left")->click();
    btn("jog_down")->click();
    btn("jog_z_down")->click();
    settle();
    QCOMPARE(lab_->x(), 2.5);
    QCOMPARE(lab_->y(), 0.0);
    QCOMPARE(lab_->z(), 0.0);
  }

  void errors_reach_the_status_line() {
    btn("jog_left")->click();  // off the stage: the travel starts at 0
    settle();
    QVERIFY2(text("status").startsWith(QStringLiteral("jog: ")), qPrintable(text("status")));
    QVERIFY(!text("status").contains(QStringLiteral("done")));
    QCOMPARE(lab_->x(), 0.0);
  }

  void watch_only_disables_all_but_the_stop() {
    choose_tray(QStringLiteral("example-9"));
    auto queue = lab_->lasers->drive(Lasers::Driver::Queue);
    QVERIFY(queue.has_value());
    QTRY_VERIFY(the<QLabel>("banner")->isVisible());
    QVERIFY2(text("banner").contains(QStringLiteral("watch only")), qPrintable(text("banner")));
    for (const char* name : {"jog_left", "jog_right", "jog_up", "jog_down", "jog_z_up", "jog_z_down", "stop_stage",
                             "enable", "fire", "stop_beam", "autocenter", "cal_add", "cal_remove", "cal_clear",
                             "pattern_run", "pattern_stop"}) {
      QVERIFY2(!btn(name)->isEnabled(), name);
    }
    QVERIFY(!the<QComboBox>("tray_choice")->isEnabled());
    QVERIFY(!the<QDoubleSpinBox>("output")->isEnabled());
    QVERIFY(btn("estop")->isEnabled());
    // a click on the tray goes nowhere
    TrayView* tray = window_->tray_view();
    QTest::mouseClick(tray, Qt::LeftButton, {}, tray->hole_center(QStringLiteral("3")).toPoint());
    settle();
    QCOMPARE(lab_->x(), 0.0);
    // but what the queue does is shown
    QVERIFY(lab_->system().set_xy(4, 6).has_value());
    QTRY_VERIFY2(text("position").contains(QStringLiteral("y 6.000")), qPrintable(text("position")));
    queue->release();
    QTRY_VERIFY(!the<QLabel>("banner")->isVisible());
    QTRY_VERIFY(btn("jog_left")->isEnabled());
  }

  // Typing a new output while the beam is on sends it on Enter, and only then:
  // not when the box merely loses the focus half typed.
  void a_new_output_is_sent_on_enter_only() {
    btn("enable")->click();
    settle();
    QTRY_VERIFY(btn("fire")->isEnabled());
    auto* output = the<QDoubleSpinBox>("output");
    output->setValue(10);
    btn("fire")->click();
    settle();
    QTRY_VERIFY(bridge_->state().firing.value_or(false));
    QCOMPARE(lab_->sim().output(), 10.0);
    output->setFocus();
    output->setValue(80);  // on the way to 8.0
    the<QDoubleSpinBox>("jog_step")->setFocus();  // the focus goes elsewhere
    settle();
    QCOMPARE(lab_->sim().output(), 10.0);
    output->setFocus();
    output->setValue(8);
    QTest::keyClick(output, Qt::Key_Return);
    settle();
    QCOMPARE(lab_->sim().output(), 8.0);
  }

  // A window closed with the beam on closes the beam: there is then nothing
  // on screen to close it with.
  void closing_the_window_closes_the_beam() {
    btn("enable")->click();
    the<QDoubleSpinBox>("output")->setValue(12);
    settle();
    QTRY_VERIFY(btn("fire")->isEnabled());
    btn("fire")->click();
    settle();
    QTRY_VERIFY(bridge_->state().firing.value_or(false));
    window_->close();
    settle();
    QVERIFY(!lab_->sim().firing());
    QCOMPARE(lab_->sim().output(), 0.0);
  }

  void reset_is_not_offered_while_a_queue_drives() {
    auto queue = lab_->lasers->drive(Lasers::Driver::Queue);
    QVERIFY(queue.has_value());
    QTRY_VERIFY(bridge_->watch_only());
    btn("estop")->click();
    settle();
    QTRY_VERIFY2(text("banner").contains(QStringLiteral("Emergency stop")), qPrintable(text("banner")));
    QVERIFY(!btn("reset_stop")->isVisible());
    queue->release();
    QTRY_VERIFY(btn("reset_stop")->isVisible());
  }

  void emergency_stop_latches_and_reset_clears() {
    build(5);
    btn("enable")->click();
    the<QDoubleSpinBox>("output")->setValue(40);
    settle();
    QTRY_VERIFY(btn("fire")->isEnabled());
    btn("fire")->click();
    the<QDoubleSpinBox>("jog_step")->setValue(10);
    btn("jog_right")->click();
    QVERIFY(test::under_way(*lab_, 0.5));
    btn("estop")->click();
    settle();
    QVERIFY(!lab_->sim().firing());
    QVERIFY(!lab_->sim().enabled());
    QCOMPARE(lab_->sim().output(), 0.0);
    QVERIFY(lab_->x() < 10.0);
    QTRY_VERIFY(btn("reset_stop")->isVisible());
    QVERIFY2(text("banner").contains(QStringLiteral("Emergency stop")), qPrintable(text("banner")));
    for (const char* name : {"jog_left", "enable", "fire", "pattern_run", "cal_add"}) {
      QVERIFY2(!btn(name)->isEnabled(), name);
    }
    QVERIFY(btn("estop")->isEnabled());
    btn("reset_stop")->click();
    settle();
    QTRY_VERIFY(!btn("reset_stop")->isVisible());
    QTRY_VERIFY(btn("enable")->isEnabled());
    QVERIFY(!the<QLabel>("banner")->isVisible());
    QVERIFY(!btn("fire")->isEnabled());  // enable, then fire: it is off again
  }

  void calibration_flow_writes_the_file_and_shows_cautions() {
    choose_tray(QStringLiteral("example-9"));
    btn("cal_clear")->click();
    settle();
    QTRY_COMPARE(the<QTableWidget>("cal_table")->rowCount(), 0);
    QVERIFY(!std::filesystem::exists(lab_->dir / "stage_calibrations" / "co2.example-9.toml"));
    QVERIFY(!window_->tray_view()->stage_point().has_value());
    // the map's centre hole is offered first
    auto* hole = the<QComboBox>("cal_hole");
    QCOMPARE(hole->currentText(), QStringLiteral("5"));

    the<QDoubleSpinBox>("jog_step")->setValue(10);
    btn("jog_right")->click();
    btn("jog_up")->click();
    settle();
    btn("cal_add")->click();  // hole 5 at stage (10, 10)
    settle();
    QTRY_COMPARE(the<QTableWidget>("cal_table")->rowCount(), 1);
    the<QDoubleSpinBox>("jog_step")->setValue(5);
    btn("jog_up")->click();
    settle();
    hole->setCurrentText(QStringLiteral("2"));  // north of the centre
    btn("cal_add")->click();
    settle();
    auto* points = the<QTableWidget>("cal_table");
    QTRY_COMPARE(points->rowCount(), 2);
    QCOMPARE(points->item(1, 0)->text(), QStringLiteral("2"));
    QCOMPARE(points->item(1, 2)->text(), QStringLiteral("15.000"));
    QVERIFY(std::filesystem::exists(lab_->dir / "stage_calibrations" / "co2.example-9.toml"));
    QVERIFY2(text("cal_solution").contains(QStringLiteral("Centre 10.000, 10.000")), qPrintable(text("cal_solution")));
    QVERIFY2(text("cal_solution").contains(QStringLiteral("rotation 0.00")), qPrintable(text("cal_solution")));
    QVERIFY2(!text("cal_cautions").isEmpty(), "two points: a mirror is not ruled out");
    QTRY_VERIFY(window_->tray_view()->stage_point().has_value());

    // a point that cannot be right is refused and said
    hole->setCurrentText(QStringLiteral("9"));
    btn("cal_add")->click();  // the stage is still on hole 2
    settle();
    QCOMPARE(points->rowCount(), 2);
    QVERIFY2(text("status").startsWith(QStringLiteral("add calibration point: ")), qPrintable(text("status")));

    points->selectRow(1);
    QTRY_VERIFY(btn("cal_remove")->isEnabled());
    btn("cal_remove")->click();
    settle();
    QTRY_COMPARE(points->rowCount(), 1);
  }

  void patterns_are_listed_with_length_and_time() {
    auto* list = the<QTableWidget>("pattern_list");
    QCOMPARE(list->rowCount(), 2);
    QCOMPARE(list->item(0, 0)->text(), QStringLiteral("follow"));
    QCOMPARE(list->item(0, 1)->text(), QStringLiteral("dragonfly"));
    QCOMPARE(list->item(1, 0)->text(), QStringLiteral("hexagon"));
    QCOMPARE(list->item(1, 1)->text(), QStringLiteral("polygon"));
    QVERIFY2(list->item(1, 2)->text().endsWith(QStringLiteral(" mm")), qPrintable(list->item(1, 2)->text()));
    QVERIFY2(list->item(1, 3)->text().endsWith(QStringLiteral(" s")), qPrintable(list->item(1, 3)->text()));
    QVERIFY(!btn("pattern_run")->isEnabled());  // nothing chosen
  }

  void a_pattern_runs_and_a_dragonfly_cannot_be_run_by_hand() {
    auto* list = the<QTableWidget>("pattern_list");
    list->selectRow(0);  // follow: a dragonfly
    QVERIFY(!btn("pattern_run")->isEnabled());
    QVERIFY(btn("pattern_run")->toolTip().contains(QStringLiteral("queue")));
    the<QDoubleSpinBox>("jog_step")->setValue(10);
    btn("jog_right")->click();
    btn("jog_up")->click();
    settle();
    list->selectRow(1);  // hexagon
    QTRY_VERIFY(btn("pattern_run")->isEnabled());
    const auto before = lab_->sim().log().size();
    btn("pattern_run")->click();
    settle();
    QVERIFY2(text("status").contains(QStringLiteral("run pattern: done")), qPrintable(text("status")));
    int moves = 0;
    const auto log = lab_->sim().log();
    for (std::size_t i = before; i < log.size(); ++i) moves += log[i].starts_with("Stage.MoveTo") ? 1 : 0;
    QCOMPARE(moves, 8);  // six sides closed, and back to the centre
    QCOMPARE(lab_->x(), 10.0);
  }

  void snapshot_and_measure_scale_buttons() {
    btn("snapshot")->click();
    settle();
    QVERIFY2(text("status").contains(QStringLiteral("snapshots")), qPrintable(text("status")));
    QVERIFY(std::filesystem::exists(lab_->dir / "snapshots" / "co2"));

    choose_tray(QStringLiteral("example-9"));
    the<QCheckBox>("centre_on_go")->setChecked(false);
    TrayView* tray = window_->tray_view();
    QTest::mouseClick(tray, Qt::LeftButton, {}, tray->hole_center(QStringLiteral("5")).toPoint());
    settle();
    btn("measure_scale")->click();
    settle();
    QTRY_VERIFY2(text("camera_scale").contains(QStringLiteral("px/mm")), qPrintable(text("camera_scale")));
    QVERIFY2(text("camera_scale").contains(QStringLiteral("23.")), qPrintable(text("camera_scale")));
    QVERIFY(std::filesystem::exists(lab_->dir / "camera_scales" / "co2.toml"));
    // neither is offered while a queue drives
    auto queue = lab_->lasers->drive(Lasers::Driver::Queue);
    QVERIFY(queue.has_value());
    QTRY_VERIFY(!btn("measure_scale")->isEnabled());
    QVERIFY(btn("snapshot")->isEnabled());  // looking is always allowed
  }

  void the_pattern_maker_button_is_offered_when_there_is_one() {
    QVERIFY(btn("pattern_maker")->isHidden());
    int opened = 0;
    window_->set_pattern_maker([&opened] { ++opened; });
    QVERIFY(!btn("pattern_maker")->isHidden());
    btn("pattern_maker")->click();
    QCOMPARE(opened, 1);
  }

 private:
  std::unique_ptr<test::SimLaserLab> lab_;
  std::unique_ptr<LaserBridge> bridge_;
  std::unique_ptr<LaserWindow> window_;
};

// The laser in the main window's View menu.
class MainWindowLaserTest : public QObject {
  Q_OBJECT

 private slots:
  void the_main_window_offers_the_laser() {
    test::SimLaserLab lab;
    {
      MainWindow main(*lab.line);
      QVERIFY(main.laser_action() != nullptr);
      QVERIFY(!main.laser_action()->isEnabled());  // until there is one
      QVERIFY(!main.laser_action()->icon().isNull());
      QVERIFY(!main.laser_action()->shortcut().isEmpty());

      LaserBridge bridge(lab.deps());
      main.set_lasers({&bridge}, true, &lab.lab.patterns, lab.dir / "patterns");
      QVERIFY(main.laser_action()->isEnabled());
      QVERIFY(main.laser_window(QStringLiteral("co2")) == nullptr);  // not until asked for
      main.laser_action()->trigger();
      LaserWindow* window = main.laser_window(QStringLiteral("co2"));
      QVERIFY(window != nullptr);
      QVERIFY(window->isVisible());
      QCOMPARE(window->windowTitle(), QStringLiteral("Laser co2 (Simulation)"));
      main.laser_action()->trigger();
      QCOMPARE(main.laser_window(QStringLiteral("co2")), window);  // the same one again

      // its pattern maker saves into the lab, and the window lists what it saved
      auto* maker_button = window->findChild<QPushButton*>(QStringLiteral("pattern_maker"));
      QVERIFY(maker_button != nullptr && !maker_button->isHidden());
      maker_button->click();
      PatternMakerWindow* maker = main.pattern_maker();
      QVERIFY(maker != nullptr);
      QVERIFY(maker->isVisible());
      maker->findChild<QLineEdit*>(QStringLiteral("name"))->setText(QStringLiteral("made"));
      maker->findChild<QPushButton*>(QStringLiteral("save"))->click();
      QVERIFY(std::filesystem::exists(lab.dir / "patterns" / "made.toml"));
      auto* list = window->findChild<QTableWidget*>(QStringLiteral("pattern_list"));
      QCOMPARE(list->rowCount(), 3);
      QCOMPARE(list->item(2, 0)->text(), QStringLiteral("made"));

      // gone with the bridges
      main.set_lasers({}, false);
      QVERIFY(main.laser_window(QStringLiteral("co2")) == nullptr);
      QVERIFY(main.pattern_maker() == nullptr);
      QVERIFY(!main.laser_action()->isEnabled());
    }
  }
};

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  int failed = 0;
  {
    LaserWindowTest test;
    failed += QTest::qExec(&test, argc, argv);
  }
  {
    MainWindowLaserTest test;
    failed += QTest::qExec(&test, argc, argv);
  }
  return failed;
}
#include "test_laser_window.moc"
