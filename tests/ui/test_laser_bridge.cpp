// LaserBridge on the example lab's simulated laser (laser window design,
// section 5).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "laser_fixture.hpp"
#include "pychron/core/clock.hpp"
#include "pychron/core/log_hub.hpp"
#include "pychron/core/process_log.hpp"
#include "pychron/core/virtual_clock.hpp"

using namespace pychron;
using namespace pychron::ui;
using pychron::experiment::lab::Lasers;

namespace {

// What a bridge reported, in order.
struct Heard {
  std::vector<std::pair<QString, Result<void>>> finished;
  std::vector<laser::LaserSnapshot> snapshots;
  int views = 0;
  int calibrations = 0;
  std::vector<bool> drivers;

  explicit Heard(LaserBridge& bridge) {
    QObject::connect(&bridge, &LaserBridge::commandFinished,
                     [this](const QString& what, const Result<void>& r) { finished.emplace_back(what, r); });
    QObject::connect(&bridge, &LaserBridge::snapshot,
                     [this](const laser::LaserSnapshot& s) { snapshots.push_back(s); });
    QObject::connect(&bridge, &LaserBridge::view, [this](const laser::CameraView&) { ++views; });
    QObject::connect(&bridge, &LaserBridge::calibrationChanged, [this] { ++calibrations; });
    QObject::connect(&bridge, &LaserBridge::driverChanged, [this](bool w) { drivers.push_back(w); });
  }
  const Result<void>* result(const QString& what) const {
    for (const auto& it : std::views::reverse(finished)) {
      if (it.first == what) return &it.second;
    }
    return nullptr;
  }
  bool ok(const QString& what) const {
    const auto* r = result(what);
    return r != nullptr && r->has_value();
  }
  QString why(const QString& what) const {
    const auto* r = result(what);
    if (r == nullptr) return QStringLiteral("(never finished)");
    return r->has_value() ? QString() : QString::fromStdString(r->error().what);
  }
};

}  // namespace

class LaserBridgeTest : public QObject {
  Q_OBJECT

 private slots:
  void init() { build(50); }
  // The same again with time running at `speed`: slowly, for a test that
  // stops something part way.
  void build(double speed) {
    cleanup();
    lab_ = std::make_unique<test::SimLaserLab>(speed);
    bridge_ = std::make_unique<LaserBridge>(lab_->deps([this] { ++aborts_; }));
    heard_ = std::make_unique<Heard>(*bridge_);
    aborts_ = 0;
  }
  void cleanup() {
    heard_.reset();
    bridge_.reset();
    on_video_clock_.reset();
    video_clock_.reset();
    lab_.reset();
  }

  void says_what_the_lab_has() {
    QCOMPARE(bridge_->device(), QStringLiteral("co2"));
    QVERIFY(bridge_->trays().contains(QStringLiteral("example-9")));
    QVERIFY(bridge_->tray_map(QStringLiteral("example-9")) != nullptr);
    QVERIFY(bridge_->tray_map(QStringLiteral("nope")) == nullptr);
    QVERIFY(bridge_->has_camera());
    QCOMPARE(bridge_->calibration(QStringLiteral("example-9")).state, laser::CalibrationState::Ok);
    QCOMPARE(bridge_->calibration_points(QStringLiteral("example-9")).size(), std::size_t{2});
  }

  void goes_to_a_hole() {
    bridge_->set_tray(QStringLiteral("example-9"));
    bridge_->go_to(QStringLiteral("3"), false);
    test::settle(*bridge_);
    QVERIFY2(heard_->ok("set_tray"), qPrintable(heard_->why("set_tray")));
    QVERIFY2(heard_->ok("go_to"), qPrintable(heard_->why("go_to")));
    // example-9 is calibrated with its center at stage (25, 25): hole 3 is at (30, 30)
    QCOMPARE(lab_->x(), 30.0);
    QCOMPARE(lab_->y(), 30.0);
    QCOMPARE(bridge_->state().last_hole, std::string("3"));
    QCOMPARE(bridge_->state().tray, std::string("example-9"));
    QCOMPARE(bridge_->state().activity, laser::LaserActivity::Idle);
  }

  void go_to_centers_when_asked() {
    bridge_->set_tray(QStringLiteral("example-9"));
    bridge_->go_to(QStringLiteral("3"), true);
    test::settle(*bridge_);
    QVERIFY2(heard_->ok("go_to"), qPrintable(heard_->why("go_to")));
    // the example's camera sees the tray 0.15, -0.10 mm from its calibration
    QVERIFY(std::abs(lab_->x() - 30.15) < 0.04);
    QVERIFY(std::abs(lab_->y() - 29.90) < 0.04);
    QCOMPARE(bridge_->state().autocenter.result, laser::AutocenterOutcome::Result::Converged);
    QVERIFY(bridge_->corrected_holes().contains(QStringLiteral("3")));
  }

  void a_hole_with_no_tray_is_refused() {
    bridge_->go_to(QStringLiteral("3"), false);
    test::settle(*bridge_);
    QVERIFY(!heard_->ok("go_to"));
    QVERIFY2(heard_->why("go_to").contains("no tray"), qPrintable(heard_->why("go_to")));
  }

  void jog_moves_by_the_step() {
    bridge_->jog(1.5, 0.5, 0);
    bridge_->jog(0, 0, 2);
    bridge_->jog(-0.5, 0, 0);
    test::settle(*bridge_);
    QVERIFY2(heard_->ok("jog"), qPrintable(heard_->why("jog")));
    // The driver counts the stage arrived within its tolerance; the simulated
    // stage is on the target itself a moment later.
    QTRY_COMPARE(lab_->x(), 1.0);
    QTRY_COMPARE(lab_->y(), 0.5);
    QTRY_COMPARE(lab_->z(), 2.0);
  }

  void a_jog_off_the_stage_is_refused_and_says_why() {
    bridge_->jog(-5, 0, 0);
    test::settle(*bridge_);
    QVERIFY(!heard_->ok("jog"));
    QCOMPARE(lab_->x(), 0.0);
  }

  void stop_stage_ends_a_move() {
    build(5);
    bridge_->jog(40, 0, 0);  // 8 simulated seconds at 5 mm/s
    QVERIFY(test::under_way(*lab_));
    bridge_->stop_stage();
    test::settle(*bridge_);
    const Result<void>* jog = heard_->result("jog");
    QVERIFY(jog != nullptr);
    QVERIFY(!jog->has_value());
    QCOMPARE(jog->error().kind, ErrorKind::Cancelled);
    QVERIFY(heard_->ok("stop_stage"));
    QVERIFY2(lab_->x() > 0.0 && lab_->x() < 40.0, qPrintable(QString::number(lab_->x())));
    const double where = lab_->x();
    QTest::qWait(100);
    QCOMPARE(lab_->x(), where);
  }

  void enable_fire_stop() {
    bridge_->enable(true);
    bridge_->fire(15);
    test::settle(*bridge_);
    QVERIFY2(heard_->ok("fire"), qPrintable(heard_->why("fire")));
    QVERIFY(lab_->sim().enabled());
    QVERIFY(lab_->sim().firing());
    QCOMPARE(lab_->sim().output(), 15.0);
    QCOMPARE(bridge_->state().firing, std::optional<bool>(true));
    bridge_->set_output(22);
    test::settle(*bridge_);
    QCOMPARE(lab_->sim().output(), 22.0);
    QVERIFY(lab_->sim().firing());
    bridge_->stop_beam();
    test::settle(*bridge_);
    QVERIFY(heard_->ok("stop_beam"));
    QVERIFY(!lab_->sim().firing());
    QCOMPARE(lab_->sim().output(), 0.0);
    bridge_->enable(false);
    test::settle(*bridge_);
    QVERIFY(!lab_->sim().enabled());
  }

  void fire_needs_enable() {
    bridge_->fire(15);
    test::settle(*bridge_);
    QVERIFY(!heard_->ok("fire"));
    QVERIFY(!lab_->sim().firing());
    QCOMPARE(lab_->sim().output(), 0.0);
  }

  void an_interlock_refuses_fire_and_is_named() {
    bridge_->enable(true);
    test::settle(*bridge_);
    lab_->sim().trip_interlock("Door");
    bridge_->fire(15);
    test::settle(*bridge_);
    QVERIFY(!heard_->ok("fire"));
    QVERIFY2(heard_->why("fire").contains("Door"), qPrintable(heard_->why("fire")));
    QVERIFY(!lab_->sim().firing());
    QCOMPARE(lab_->sim().output(), 0.0);  // no output left standing with no beam
    QCOMPARE(bridge_->state().interlocks, (std::vector<std::string>{"Door"}));
  }

  void runs_and_stops_a_pattern() {
    bridge_->jog(10, 10, 0);
    bridge_->run_pattern(QStringLiteral("hexagon"));
    test::settle(*bridge_);
    QVERIFY2(heard_->ok("run_pattern"), qPrintable(heard_->why("run_pattern")));
    QCOMPARE(lab_->x(), 10.0);  // back at its center
    QVERIFY(std::count_if(heard_->snapshots.begin(), heard_->snapshots.end(), [](const laser::LaserSnapshot& s) {
              return s.activity == laser::LaserActivity::Pattern;
            }) > 0);

  }

  void a_pattern_is_stopped_part_way() {
    build(5);  // the hexagon takes 7 simulated seconds: over a second here
    bridge_->jog(10, 10, 0);
    test::settle(*bridge_);
    bridge_->run_pattern(QStringLiteral("hexagon"));
    QTRY_COMPARE(bridge_->state().activity, laser::LaserActivity::Pattern);
    bridge_->stop_pattern();
    test::settle(*bridge_);
    const Result<void>* run = heard_->result("run_pattern");
    QVERIFY(run != nullptr);
    QVERIFY2(!run->has_value(), "the pattern ended before it was stopped");
    QCOMPARE(run->error().kind, ErrorKind::Cancelled);
    QVERIFY(heard_->ok("stop_pattern"));
    QCOMPARE(bridge_->state().activity, laser::LaserActivity::Idle);
  }

 private:
  // Follow (simulated): the dragonfly grain simulator design, section 8.
  void build_to_follow(const std::function<void(const std::filesystem::path&)>& prepare = {},
                       const std::function<bool(std::string_view)>& simulated = {}) {
    cleanup();
    lab_ = std::make_unique<test::SimLaserLab>(
        50,
        [&prepare](const std::filesystem::path& dir) {
          test::add_long_dragonfly(dir);
          if (prepare) prepare(dir);
        },
        simulated);
    bridge_ = std::make_unique<LaserBridge>(lab_->deps([this] { ++aborts_; }));
    heard_ = std::make_unique<Heard>(*bridge_);
    aborts_ = 0;
  }
  // A spiral from the stage's corner would leave its travel at once.
  void on_a_hole() {
    bridge_->set_tray(QStringLiteral("example-9"));
    bridge_->go_to(QStringLiteral("5"), false);
    test::settle(*bridge_);
    QVERIFY2(heard_->ok("go_to"), qPrintable(heard_->why("go_to")));
  }
  bool ever_sent(const char* command) {
    const auto log = lab_->sim().log();
    return std::ranges::any_of(log, [command](const std::string& line) { return line.starts_with(command); });
  }
  bool laser_is_off() {
    return !lab_->sim().firing() && !lab_->sim().enabled() && lab_->sim().output() == 0.0;
  }

  void following() {
    build_to_follow();
    on_a_hole();
    bridge_->follow_simulated(QStringLiteral("watch"), 20);
    QTRY_VERIFY(lab_->sim().firing());
    QTRY_VERIFY(lab_->system().looking());
  }
  void stopped_and_off(const char* by) {
    test::settle(*bridge_);
    QCOMPARE(heard_->result("follow_simulated")->error().kind, ErrorKind::Cancelled);
    QVERIFY(heard_->ok(by));
    QVERIFY(laser_is_off());
    QVERIFY(!lab_->system().looking());
  }

 private slots:
  void a_simulated_laser_offers_to_follow() { QVERIFY(bridge_->offers_simulated_follow()); }

  void a_real_laser_does_not_offer_to_follow_and_refuses_it() {
    build_to_follow({}, [](std::string_view) { return false; });
    QVERIFY(!bridge_->offers_simulated_follow());
    bridge_->follow_simulated(QStringLiteral("watch"), 20);
    test::settle(*bridge_);
    QVERIFY2(heard_->why("follow_simulated").contains("simulated"), qPrintable(heard_->why("follow_simulated")));
    QVERIFY(!lab_->sim().enabled());
    QVERIFY(!ever_sent("Laser.Fire"));
  }

  void a_camera_for_looking_does_not_offer_to_follow() {
    build_to_follow([](const std::filesystem::path& dir) {
      const auto file = dir / "cameras.toml";
      std::ifstream in(file);
      std::stringstream text;
      text << in.rdbuf();
      in.close();
      std::string cameras = text.str();
      const auto at = cameras.find("\nuse = \"center\"");
      QVERIFY(at != std::string::npos);
      cameras.replace(at, 15, "\nuse = \"view\"");
      std::ofstream(file) << cameras;
    });
    QVERIFY(bridge_->has_camera());
    QVERIFY(!bridge_->offers_simulated_follow());
  }

  void only_a_dragonfly_is_followed() {
    bridge_->follow_simulated(QStringLiteral("hexagon"), 20);
    bridge_->follow_simulated(QStringLiteral("nope"), 20);
    test::settle(*bridge_);
    QVERIFY2(heard_->why("follow_simulated").contains("not a dragonfly"), qPrintable(heard_->why("follow_simulated")));
    QVERIFY(!ever_sent("Laser.Fire"));
  }

  double perimeter_of_a_follow(double fence_mm) {
    std::vector<laser::FollowSample> rows;
    QObject guard;
    connect(bridge_.get(), &LaserBridge::followSamples, &guard,
            [&rows](const std::vector<laser::FollowSample>& got) { rows.insert(rows.end(), got.begin(), got.end()); });
    bridge_->follow_simulated(QStringLiteral("follow"), 20, fence_mm);
    test::settle(*bridge_);
    QCoreApplication::processEvents();
    return rows.empty() ? 0.0 : rows.front().step.perimeter_mm;
  }

  void the_fence_is_set_and_the_hole_limits_it() {
    on_a_hole();
    const double room = bridge_->state().hole_room_mm;
    QVERIFY(room > 0);
    const double held = perimeter_of_a_follow(0.5 * room);
    QVERIFY(std::abs(held - 0.5 * room) < 1e-9);
    const double capped = perimeter_of_a_follow(100);  // the second on this hole: it is still known
    QVERIFY(std::abs(capped - room) < 1e-9);
    QCOMPARE(bridge_->state().last_hole, std::string("5"));
    bridge_->jog(0.1, 0, 0);
    test::settle(*bridge_);
    QTRY_COMPARE(bridge_->state().hole_room_mm, 0.0);  // off the hole, for all it knows
  }

  void a_reset_is_refused_on_a_real_laser() {
    build_to_follow({}, [](std::string_view) { return false; });
    bridge_->reset_simulation(false);
    test::settle(*bridge_);
    QVERIFY2(heard_->why("reset_simulation").contains("simulated"), qPrintable(heard_->why("reset_simulation")));
  }

  void a_reset_goes_back_to_the_last_hole() {
    on_a_hole();
    const auto hole = lab_->sim().position();
    bridge_->follow_simulated(QStringLiteral("follow"), 20);
    test::settle(*bridge_);
    QVERIFY2(heard_->ok("follow_simulated"), qPrintable(heard_->why("follow_simulated")));
    bridge_->reset_simulation(false);
    test::settle(*bridge_);
    QVERIFY2(heard_->ok("reset_simulation"), qPrintable(heard_->why("reset_simulation")));
    QCOMPARE(lab_->sim().position().x, hole.x);
    QCOMPARE(lab_->sim().position().y, hole.y);
  }

  void a_jog_means_no_hole_to_go_back_to() {
    on_a_hole();
    bridge_->jog(0.4, 0.3, 0);
    test::settle(*bridge_);
    const auto moved = lab_->sim().position();
    bridge_->reset_simulation(false);
    test::settle(*bridge_);
    QVERIFY2(heard_->ok("reset_simulation"), qPrintable(heard_->why("reset_simulation")));
    QCOMPARE(lab_->sim().position().x, moved.x);
  }

  void a_follow_fires_at_its_output_and_ends_off() {
    on_a_hole();
    bridge_->follow_simulated(QStringLiteral("follow"), 20);
    QTRY_VERIFY(lab_->sim().firing());
    QCOMPARE(lab_->sim().output(), 20.0);
    test::settle(*bridge_);
    QVERIFY2(heard_->ok("follow_simulated"), qPrintable(heard_->why("follow_simulated")));
    QVERIFY(laser_is_off());
  }

  void stopping_the_pattern_turns_the_follow_off() {
    following();
    bridge_->stop_pattern();
    stopped_and_off("stop_pattern");
  }

  void stopping_the_beam_turns_the_follow_off() {
    following();
    bridge_->stop_beam();
    stopped_and_off("stop_beam");
  }

  void stopping_the_stage_turns_the_follow_off() {
    following();
    bridge_->stop_stage();
    stopped_and_off("stop_stage");
  }

  void an_emergency_stop_turns_the_follow_off() {
    following();
    bridge_->emergency_stop();
    stopped_and_off("emergency_stop");
    QCOMPARE(aborts_, 1);
  }

  void a_bridge_that_goes_turns_the_follow_off() {
    following();
    heard_.reset();
    bridge_.reset();
    QVERIFY(laser_is_off());
  }

  void a_stage_that_fails_turns_the_follow_off() {
    build_to_follow();
    on_a_hole();
    for (int i = 0; i < 8; ++i) lab_->sim().fail_next("Stage.MoveTo", 4);  // the line's transport tries again
    bridge_->follow_simulated(QStringLiteral("watch"), 20);
    test::settle(*bridge_);
    QVERIFY2(heard_->why("follow_simulated").contains("pattern watch"), qPrintable(heard_->why("follow_simulated")));
    QVERIFY(ever_sent("Laser.Fire"));
    QVERIFY(laser_is_off());
  }

  void a_refused_fire_leaves_the_laser_off() {
    build_to_follow();
    lab_->sim().trip_interlock("Door");
    bridge_->follow_simulated(QStringLiteral("watch"), 20);
    test::settle(*bridge_);
    QVERIFY(!heard_->why("follow_simulated").isEmpty());
    QVERIFY(!lab_->sim().firing());
    QVERIFY(!lab_->sim().enabled());
  }

  void a_beam_that_will_not_stop_is_closed_by_the_disable() {
    following();
    for (int i = 0; i < 2; ++i) lab_->sim().fail_next("Laser.Stop", 4);  // end_extract's own is refused
    bridge_->stop_pattern();
    test::settle(*bridge_);
    QVERIFY(laser_is_off());
  }

  void follow_reaches_the_main_thread() {
    build_to_follow(test::add_grain);
    on_a_hole();
    std::vector<laser::FollowSample> rows;
    bool on_main = true;
    connect(bridge_.get(), &LaserBridge::followSamples, this,
            [&rows, &on_main](const std::vector<laser::FollowSample>& got) {
              on_main = on_main && QThread::currentThread() == QCoreApplication::instance()->thread();
              rows.insert(rows.end(), got.begin(), got.end());
            });
    bridge_->follow_simulated(QStringLiteral("watch"), 20);
    QTRY_VERIFY(rows.size() >= 2);
    QVERIFY(on_main);
    QVERIFY(rows.back().step.elapsed_s > rows.front().step.elapsed_s);
    bridge_->stop_pattern();
    test::settle(*bridge_);
    QVERIFY(laser_is_off());
  }

  void a_bridge_gone_with_a_dragonfly_running_is_safe() {
    following();
    bridge_.reset();  // mid-Follow: the observer goes first, the worker next
    heard_.reset();
    QCoreApplication::processEvents();
  }

  void the_grain_is_shown_only_while_something_looks_for_it() {
    build_to_follow(test::add_grain);
    on_a_hole();
    int with = 0, without = 0;
    connect(bridge_.get(), &LaserBridge::view, this,
            [&with, &without](const laser::CameraView& seen) { ++(seen.grain_px ? with : without); });
    bridge_->add_viewer();
    QTRY_VERIFY(without > 0);
    QCOMPARE(with, 0);
    bridge_->follow_simulated(QStringLiteral("watch"), 20);
    QTRY_VERIFY(with > 0);
    bridge_->stop_pattern();
    test::settle(*bridge_);
    bridge_->remove_viewer();
    QVERIFY(laser_is_off());
  }

  void no_grain_table_no_grain_shown() {
    build_to_follow();
    on_a_hole();
    int with = 0, frames = 0;
    connect(bridge_.get(), &LaserBridge::view, this, [&with, &frames](const laser::CameraView& seen) {
      ++frames;
      if (seen.grain_px) ++with;
    });
    bridge_->add_viewer();
    bridge_->follow_simulated(QStringLiteral("watch"), 20);
    QTRY_VERIFY(lab_->system().looking());
    const int at = frames;
    QTRY_VERIFY(frames > at + 2);
    bridge_->stop_pattern();
    test::settle(*bridge_);
    bridge_->remove_viewer();
    QCOMPARE(with, 0);
  }

  void an_unknown_pattern_is_refused() {
    bridge_->run_pattern(QStringLiteral("nope"));
    test::settle(*bridge_);
    QVERIFY2(heard_->why("run_pattern").contains("unknown pattern"), qPrintable(heard_->why("run_pattern")));
  }

  void calibration_point_is_saved_and_used() {
    bridge_->set_tray(QStringLiteral("example-9"));
    bridge_->clear_calibration();
    test::settle(*bridge_);
    QVERIFY2(heard_->ok("clear_calibration"), qPrintable(heard_->why("clear_calibration")));
    QCOMPARE(bridge_->calibration(QStringLiteral("example-9")).state, laser::CalibrationState::Missing);
    QCOMPARE(bridge_->state().calibration, laser::CalibrationState::Missing);

    // the center hole (5) is put at stage (12, 14), then its east neighbour (6), 5 mm along x
    bridge_->jog(12, 14, 0);
    bridge_->add_calibration_point(QStringLiteral("5"));
    bridge_->jog(5, 0, 0);
    bridge_->add_calibration_point(QStringLiteral("6"));
    test::settle(*bridge_);
    QVERIFY2(heard_->ok("add_calibration_point"), qPrintable(heard_->why("add_calibration_point")));
    QVERIFY(heard_->calibrations >= 3);
    const auto points = bridge_->calibration_points(QStringLiteral("example-9"));
    QCOMPARE(points.size(), std::size_t{2});
    QCOMPARE(points[0].hole, std::string("5"));
    QCOMPARE(points[0].x, 12.0);
    QCOMPARE(points[1].x, 17.0);
    QCOMPARE(bridge_->state().calibration, laser::CalibrationState::Ok);

    // and a move uses it at once: hole 5 is where it was put
    bridge_->jog(3, 3, 0);
    bridge_->go_to(QStringLiteral("5"), false);
    test::settle(*bridge_);
    QVERIFY2(heard_->ok("go_to"), qPrintable(heard_->why("go_to")));
    QCOMPARE(lab_->x(), 12.0);
    QCOMPARE(lab_->y(), 14.0);

    bridge_->remove_calibration_point(QStringLiteral("6"));
    test::settle(*bridge_);
    QCOMPARE(bridge_->calibration_points(QStringLiteral("example-9")).size(), std::size_t{1});
    bridge_->remove_calibration_point(QStringLiteral("5"));
    test::settle(*bridge_);
    QCOMPARE(bridge_->calibration(QStringLiteral("example-9")).state, laser::CalibrationState::Missing);
  }

  void a_calibration_point_that_does_not_solve_is_refused() {
    bridge_->set_tray(QStringLiteral("example-9"));
    test::settle(*bridge_);
    const auto before = bridge_->calibration_points(QStringLiteral("example-9"));
    // the stage has not moved: a second hole at the place of none, far from where the first two put it
    bridge_->add_calibration_point(QStringLiteral("1"));
    test::settle(*bridge_);
    QVERIFY(!heard_->ok("add_calibration_point"));
    const auto after = bridge_->calibration_points(QStringLiteral("example-9"));
    QCOMPARE(after.size(), before.size());
    bridge_->add_calibration_point(QStringLiteral("no-such-hole"));
    test::settle(*bridge_);
    QVERIFY(!heard_->ok("add_calibration_point"));
  }

  void a_snapshot_is_a_picture_any_program_opens() {
    QString saved;
    QObject::connect(bridge_.get(), &LaserBridge::snapshotSaved, [&saved](const QString& file) { saved = file; });
    bridge_->snapshot_to_file();
    test::settle(*bridge_);
    QVERIFY2(heard_->ok("snapshot"), qPrintable(heard_->why("snapshot")));
    QVERIFY(saved.startsWith(QString::fromStdString((lab_->dir / "snapshots" / "co2").string())));
    // read by Qt's own decoder, not ours
    const QImage picture(saved);
    QVERIFY2(!picture.isNull(), qPrintable(saved));
    QCOMPARE(picture.size(), QSize(200, 200));
    QVERIFY(picture.isGrayscale());
    // the simulated tray is light: the picture is not black
    QVERIFY(qGray(picture.pixel(3, 3)) > 60);
  }

  void the_cameras_scale_is_measured_saved_and_used() {
    bridge_->set_tray(QStringLiteral("example-9"));
    bridge_->go_to(QStringLiteral("5"), false);
    std::optional<laser::ScaleMeasurement> measured;
    QObject::connect(bridge_.get(), &LaserBridge::scaleMeasured,
                     [&measured](const laser::ScaleMeasurement& m) { measured = m; });
    bridge_->measure_scale(0.5);
    test::settle(*bridge_);
    QVERIFY2(heard_->ok("measure_scale"), qPrintable(heard_->why("measure_scale")));
    QVERIFY(measured.has_value());
    QVERIFY(std::abs(measured->px_per_mm - 23.0) < 0.5);
    QVERIFY(!measured->flip_x);
    QVERIFY(measured->flip_y);
    QVERIFY(std::filesystem::exists(lab_->dir / "camera_scales" / "co2.toml"));
    QCOMPARE(lab_->x(), 25.0);  // put back on the hole
    // nothing to see: said, and nothing saved over the good one
    bridge_->jog(-20, -20, 0);
    bridge_->measure_scale(0.5);
    test::settle(*bridge_);
    QVERIFY2(heard_->why("measure_scale").contains("nothing to follow"), qPrintable(heard_->why("measure_scale")));
  }

  // The picture is video: a frame every `video` (25 a second), whatever else
  // the bridge is doing. Counted on a clock of the test's own, which the
  // video thread keeps time by and which moves only while this thread sleeps
  // in it: the count is exact on any machine, where a rate against real time
  // came up short on one whose timed waits overrun. The finder is not run on
  // the picture: that is for a centering or a dragonfly, while one runs.
  void the_picture_is_shown_as_video() {
    const auto period = on_a_video_clock();
    bridge_->add_viewer();  // a window shows it

    int frames = 0, with_target = 0;
    std::uint64_t last_seq = 0;
    bool in_order = true;
    int skipped = 0;  // frames the camera gave that were never shown
    QObject::connect(bridge_.get(), &LaserBridge::view, [&](const laser::CameraView& seen) {
      ++frames;
      if (seen.target) ++with_target;
      if (seen.frame.seq <= last_seq) in_order = false;
      if (last_seq != 0 && seen.frame.seq > last_seq + 1) skipped += static_cast<int>(seen.frame.seq - last_seq - 1);
      last_seq = seen.frame.seq;
    });
    // Every frame taken so far is shown: the video thread is asleep in the
    // clock again (waited for in real time), and what it posted is delivered.
    const auto a_second = [&] {
      const int before = frames;
      if (!video_second(period)) return -1;
      return frames - before;
    };
    const auto shown = [this] { return video_shown(); };
    const int every_second = static_cast<int>(std::chrono::seconds(1) / period);
    QCOMPARE(every_second, 25);

    QVERIFY(shown());
    bridge_->set_tray(QStringLiteral("example-9"));
    bridge_->go_to(QStringLiteral("5"), false);
    test::settle(*bridge_);
    QCOMPARE(a_second(), every_second);  // with nothing else going on
    // What the bridge is doing never slows the picture: a move under way.
    const double from = lab_->x();
    bridge_->jog(20, 0, 0);
    QVERIFY(test::under_way(*lab_, from + 0.5));
    QCOMPARE(a_second(), every_second);
    QCOMPARE(skipped, 0);
    QVERIFY(in_order);
    test::settle(*bridge_);
    // nothing is looked for in it: the stage sat on a hole, and no frame says so
    QCOMPARE(with_target, 0);
    bridge_->go_to(QStringLiteral("5"), false);
    test::settle(*bridge_);
    QCOMPARE(a_second(), every_second);
    QCOMPARE(with_target, 0);
  }

  // No laser view open: the video thread takes no frames, so the camera
  // never reads the stage for one and the log is not filled with it.
  void noFramesWithoutAViewer() {
    QTemporaryDir tmp;
    const auto hub = log_hub(tmp, LogLevel::Trace);
    const ProcessLogScope scope(hub);
    rebuild_lab();  // its devices take their loggers from the hub above
    const auto period = on_a_video_clock();
    QVERIFY(video_second(period));
    QCOMPARE(heard_->views, 0);
    hub->flush();
    const std::string log = slurp(hub->file_path());
    QCOMPARE(count_lines(log, "[laser.video] device."), 0);
  }

  // Frames flow while a view is open, at the configured rate; the first at
  // once, not a period later. Closed, they stop.
  void framesWhileAViewerIsOpen() {
    const auto period = on_a_video_clock();
    const int every_second = static_cast<int>(std::chrono::seconds(1) / period);
    QVERIFY(video_shown());
    QCOMPARE(heard_->views, 0);
    bridge_->add_viewer();
    QVERIFY(video_shown());
    QCOMPARE(heard_->views, 1);  // no time has passed
    int before = heard_->views;
    QVERIFY(video_second(period));
    QCOMPARE(heard_->views - before, every_second);
    bridge_->remove_viewer();
    before = heard_->views;
    QVERIFY(video_second(period));
    QCOMPARE(heard_->views - before, 0);
    // and open again: at once
    bridge_->add_viewer();
    QVERIFY(video_shown());
    QCOMPARE(heard_->views - before, 1);
    QVERIFY(video_second(period));
    QCOMPARE(heard_->views - before, 1 + every_second);
    bridge_->remove_viewer();
  }

  // Frames are polling: their device calls are logged at trace, so a log at
  // debug does not get 25 lines a second from them.
  void videoDeviceCallsAreTrace() {
    QTemporaryDir tmp;
    const auto hub = log_hub(tmp, LogLevel::Debug);
    const ProcessLogScope scope(hub);
    rebuild_lab();
    const auto period = on_a_video_clock();
    bridge_->add_viewer();
    QVERIFY(video_second(period));
    QVERIFY(heard_->views > 0);
    hub->flush();
    QCOMPARE(count_lines(slurp(hub->file_path()), "[debug] [laser.video] device."), 0);
    // They are there at trace: what was left out above was there to leave out.
    hub->set_level("device", LogLevel::Trace);
    QVERIFY(video_second(period));
    hub->flush();
    QVERIFY(count_lines(slurp(hub->file_path()), "[trace] [laser.video] device.co2: position") > 0);
    bridge_->remove_viewer();
  }

  // A bridge whose video thread waits for a viewer goes at once, on either
  // clock.
  void destroyedWhilePausedReturnsPromptly() {
    const auto bound = std::chrono::seconds(5);
    QTest::qWait(100);  // the video thread (real time) is waiting for a viewer
    auto started = std::chrono::steady_clock::now();
    heard_.reset();
    bridge_.reset();
    QVERIFY(std::chrono::steady_clock::now() - started < bound);
    (void)on_a_video_clock();
    QVERIFY(video_shown());
    started = std::chrono::steady_clock::now();
    heard_.reset();
    bridge_.reset();
    QVERIFY(std::chrono::steady_clock::now() - started < bound);
  }

  void publishes_snapshots_and_views() {
    bridge_->add_viewer();
    QTRY_VERIFY(heard_->snapshots.size() >= 3);
    QTRY_VERIFY(heard_->views >= 3);
    QCOMPARE(heard_->snapshots.back().device, std::string("co2"));
    QVERIFY(heard_->snapshots.back().position.has_value());
  }

  void refused_while_a_queue_drives() {
    auto queue = lab_->lasers->drive(Lasers::Driver::Queue);
    QVERIFY(queue.has_value());
    QTRY_VERIFY(bridge_->watch_only());
    QCOMPARE(heard_->drivers, std::vector<bool>{true});
    bridge_->jog(1, 0, 0);
    bridge_->enable(true);
    test::settle(*bridge_);
    bridge_->stop_stage();
    test::settle(*bridge_);
    for (const char* what : {"jog", "enable", "stop_stage"}) {
      QVERIFY2(!heard_->ok(what), what);
    }
    QVERIFY2(heard_->why("enable").contains("a queue is running"), qPrintable(heard_->why("enable")));
    QCOMPARE(lab_->x(), 0.0);
    QVERIFY(!lab_->sim().enabled());
    queue->release();
    QTRY_VERIFY(!bridge_->watch_only());
    bridge_->jog(1, 0, 0);
    test::settle(*bridge_);
    QCOMPARE(lab_->x(), 1.0);
  }

  void emergency_stop_works_while_a_queue_drives_and_aborts_it() {
    build(5);
    auto queue = lab_->lasers->drive(Lasers::Driver::Queue);
    QVERIFY(queue.has_value());
    // as a script would have left it
    QVERIFY(lab_->system().enable().has_value());
    QVERIFY(lab_->system().extract(30, extraction::ExtractUnits::Percent).has_value());
    QVERIFY(lab_->system().fire_laser().has_value());
    QVERIFY(lab_->system().set_xy(40, 40).has_value());
    bridge_->emergency_stop();
    test::settle(*bridge_);
    QVERIFY2(heard_->ok("emergency_stop"), qPrintable(heard_->why("emergency_stop")));
    QCOMPARE(aborts_.load(), 1);
    QVERIFY(!lab_->sim().firing());
    QVERIFY(!lab_->sim().enabled());
    QCOMPARE(lab_->sim().output(), 0.0);
    QVERIFY(lab_->x() < 40.0);
    QVERIFY(bridge_->state().stopped);
    // the script's next call is refused
    QVERIFY(!lab_->system().fire_laser().has_value());
    // and the stop cannot be reset under a queue that may not have seen its abort yet
    bridge_->reset_stop();
    test::settle(*bridge_);
    QVERIFY2(heard_->why("reset_stop").contains("queue"), qPrintable(heard_->why("reset_stop")));
    QVERIFY(lab_->system().stopped());
    QVERIFY(!lab_->system().fire_laser().has_value());
    queue->release();

    bridge_->enable(true);
    test::settle(*bridge_);
    QVERIFY2(heard_->why("enable").contains("emergency stop"), qPrintable(heard_->why("enable")));
    bridge_->reset_stop();
    bridge_->enable(true);
    test::settle(*bridge_);
    QVERIFY2(heard_->ok("enable"), qPrintable(heard_->why("enable")));
    QVERIFY(!bridge_->state().stopped);
  }

  void the_stop_is_latched_before_the_worker_gets_to_it() {
    build(5);
    bridge_->jog(40, 0, 0);
    QVERIFY(test::under_way(*lab_));
    bridge_->emergency_stop();
    // at once, on this thread: whatever the worker or the device is busy with
    QVERIFY(lab_->system().stopped());
    test::settle(*bridge_);
  }

  // Nobody is left to close a beam once its bridge has gone.
  void destroyed_with_the_beam_on_closes_it() {
    bridge_->enable(true);
    bridge_->fire(25);
    test::settle(*bridge_);
    QVERIFY(lab_->sim().firing());
    heard_.reset();
    bridge_.reset();
    QVERIFY(!lab_->sim().firing());
    QCOMPARE(lab_->sim().output(), 0.0);
    QVERIFY(!lab_->sim().enabled());
  }

  // But a queue's beam is the queue's.
  void destroyed_while_a_queue_drives_touches_nothing() {
    auto queue = lab_->lasers->drive(Lasers::Driver::Queue);
    QVERIFY(queue.has_value());
    QVERIFY(lab_->system().enable().has_value());
    QVERIFY(lab_->system().extract(30, extraction::ExtractUnits::Percent).has_value());
    QVERIFY(lab_->system().fire_laser().has_value());
    heard_.reset();
    bridge_.reset();
    QVERIFY(lab_->sim().firing());
    QVERIFY(lab_->system().end_extract().has_value());
  }

  void emergency_stop_drops_what_was_queued() {
    build(5);
    bridge_->enable(true);
    bridge_->fire(10);
    bridge_->jog(40, 0, 0);
    test::settle(*bridge_);
    bridge_->jog(-40, 0, 0);  // in flight when the stop comes
    bridge_->jog(0, 30, 0);   // queued behind it
    bridge_->fire(50);        // and this
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (lab_->x() > 39.0 && std::chrono::steady_clock::now() < until) QThread::usleep(200);
    QVERIFY(lab_->x() <= 39.0);
    bridge_->emergency_stop();
    test::settle(*bridge_);
    QVERIFY(heard_->ok("emergency_stop"));
    QCOMPARE(lab_->y(), 0.0);  // the queued move never ran
    QVERIFY(!lab_->sim().firing());
    QCOMPARE(lab_->sim().output(), 0.0);
    QVERIFY(lab_->x() > 0.0);
    int cancelled = 0;
    for (const auto& [what, r] : heard_->finished) {
      if (!r.has_value() && r.error().kind == ErrorKind::Cancelled) ++cancelled;
    }
    QCOMPARE(cancelled, 3);  // the jog in flight, and the two behind it
  }

  void destroyed_mid_move_stops_the_stage() {
    build(5);
    bridge_->jog(40, 0, 0);
    QVERIFY(test::under_way(*lab_));
    heard_.reset();
    bridge_.reset();
    const double where = lab_->x();
    QVERIFY2(where > 0.0 && where < 40.0, qPrintable(QString::number(where)));
    QTest::qWait(100);
    QCOMPARE(lab_->x(), where);
    QCOMPARE(lab_->lasers->driver(), Lasers::Driver::None);
  }

 private:
  // The lab again, from scratch (after a process log hub is set).
  void rebuild_lab() {
    cleanup();
    lab_ = std::make_unique<test::SimLaserLab>(50);
    bridge_ = std::make_unique<LaserBridge>(lab_->deps([this] { ++aborts_; }));
    heard_ = std::make_unique<Heard>(*bridge_);
  }
  // The bridge again, its video thread on a clock of the test's own which
  // this thread takes part in. Returns the video period.
  std::chrono::milliseconds on_a_video_clock() {
    heard_.reset();
    bridge_.reset();
    on_video_clock_.reset();
    video_clock_ = std::make_unique<VirtualClock>();
    on_video_clock_.emplace(*video_clock_, "test");
    auto deps = lab_->deps();
    deps.video_clock = video_clock_.get();
    const auto period = deps.video;
    bridge_ = std::make_unique<LaserBridge>(std::move(deps));
    heard_ = std::make_unique<Heard>(*bridge_);
    return period;
  }
  // Every frame taken so far is shown: the video thread is asleep in the
  // clock again (waited for in real time), and what it posted is delivered.
  bool video_shown() {
    const bool asleep = QTest::qWaitFor([this] { return video_clock_->waiters() == 1; }, 20000);
    QCoreApplication::processEvents();
    return asleep;
  }
  // One second of the video clock, a period at a time as the window's event
  // loop would see it; false if the video thread stopped.
  bool video_second(std::chrono::milliseconds period) {
    for (auto t = std::chrono::milliseconds(0); t < std::chrono::seconds(1); t += period) {
      video_clock_->sleep_for(period);
      if (!video_shown()) return false;
    }
    return true;
  }
  static std::shared_ptr<LogHub> log_hub(const QTemporaryDir& dir, LogLevel level) {
    config::LoggingConfig cfg;
    cfg.dir = std::filesystem::path(dir.path().toStdString());
    cfg.default_level = level;
    auto created = LogHub::create(cfg, steady_);
    if (!created) qFatal("no log hub: %s", to_string(created.error()).c_str());
    return std::move(*created);
  }
  static std::string slurp(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
  }
  static int count_lines(const std::string& text, std::string_view needle) {
    int n = 0;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
      if (line.find(needle) != std::string::npos) ++n;
    }
    return n;
  }

  static inline SteadyClock steady_;
  std::unique_ptr<test::SimLaserLab> lab_;
  // A clock for a bridge's video thread, and this thread's part in it: after
  // the bridge in cleanup(), which waits for that thread through it.
  std::unique_ptr<VirtualClock> video_clock_;
  std::optional<Clock::Participant> on_video_clock_;
  std::unique_ptr<LaserBridge> bridge_;
  std::unique_ptr<Heard> heard_;
  std::atomic<int> aborts_{0};
};

QTEST_MAIN(LaserBridgeTest)
#include "test_laser_bridge.moc"
