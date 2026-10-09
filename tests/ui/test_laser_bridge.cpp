// LaserBridge on the example lab's simulated laser (laser window design,
// section 5).

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <ranges>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <QtTest/QtTest>

#include "laser_fixture.hpp"

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

  // What this machine makes of the waits the video loop is built on, for a
  // rate that comes up short: the cost of a picture and of a stage poll, and
  // how long a millisecond's sleep, a 40 ms timed wait and a 10 ms event-loop
  // wait really take.
  QString timing() {
    auto& sys = lab_->system();
    QElapsedTimer t;
    t.start();
    for (int i = 0; i < 10; ++i) (void)sys.picture();
    const auto picture_us = t.nsecsElapsed() / 10000;
    t.restart();
    for (int i = 0; i < 10; ++i) (void)sys.moving();
    const auto moving_us = t.nsecsElapsed() / 10000;
    t.restart();
    for (int i = 0; i < 10; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto sleep_us = t.nsecsElapsed() / 10000;
    const auto wait_us = timed_wait_us();
    t.restart();
    QTest::qWait(10);
    const auto qwait_us = t.nsecsElapsed() / 1000;
    // A call queued from another thread, as a frame is: how long until the
    // event loop runs it.
    qint64 queued_us = 0;
    {
      QEventLoop loop;
      QObject target;
      QElapsedTimer posted;
      std::thread other([&] {
        posted.start();
        QMetaObject::invokeMethod(
            &target,
            [&] {
              queued_us = posted.nsecsElapsed() / 1000;
              loop.quit();
            },
            Qt::QueuedConnection);
      });
      QTimer::singleShot(2000, &loop, &QEventLoop::quit);
      loop.exec();
      other.join();
    }
    int qos = -1;  // the test thread's quality of service, on macOS
#if defined(__APPLE__)
    qos = static_cast<int>(qos_class_self());
#endif
    return QStringLiteral(
               "picture %1 us, moving %2 us, sleep_for(1 ms) %3 us, wait_for(40 ms) %4 us, qWait(10) %5 us, a queued "
               "call %6 us, QoS 0x%7")
        .arg(picture_us)
        .arg(moving_us)
        .arg(sleep_us)
        .arg(wait_us)
        .arg(qwait_us)
        .arg(queued_us)
        .arg(qos, 0, 16);
  }

  // How long a 40 ms timed wait really takes here, the wait the video loop
  // paces itself with.
  static qint64 timed_wait_us() {
    std::mutex m;
    std::condition_variable cv;
    std::unique_lock lock(m);
    QElapsedTimer t;
    t.start();
    cv.wait_for(lock, std::chrono::milliseconds(40), [] { return false; });
    return t.nsecsElapsed() / 1000;
  }

  // The picture is video: a dozen frames a second at least, whatever else
  // the bridge is doing. The finder is not run on it: that is for a
  // centering or a dragonfly, while one runs.
  void the_picture_is_shown_as_video() {
    bridge_->set_tray(QStringLiteral("example-9"));
    bridge_->go_to(QStringLiteral("5"), false);
    test::settle(*bridge_);
    QElapsedTimer clock;
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
    // Frames a second over the next second, watched through an event loop as
    // the window does: QTest::qWait sleeps between its looks at the queue,
    // and on a machine with coarse timers each of those sleeps is longer than
    // a frame.
    const auto rate = [&] {
      frames = 0;
      clock.start();
      QEventLoop loop;
      QTimer::singleShot(1000, &loop, &QEventLoop::quit);
      loop.exec();
      return frames * 1000.0 / static_cast<double>(clock.elapsed());
    };
    const double idle = rate();  // with nothing else going on
    bridge_->jog(20, 0, 0);      // a move under way: 4 simulated seconds
    const double moving = rate();
    const qint64 wait_us = timed_wait_us();
    const auto why = [&] {
      return QStringLiteral("%1 frames a second while moving, %2 idle, %3 frames skipped; %4")
          .arg(moving)
          .arg(idle)
          .arg(skipped)
          .arg(timing());
    };
    // What the bridge is doing never slows the picture.
    QVERIFY2(moving >= 0.8 * idle, qPrintable(why()));
    // A dozen a second, wherever the machine's timed waits keep time. Where a
    // 40 ms wait takes 50 or more (the macOS CI runners, which coalesce timers
    // by tens of milliseconds) no loop paced by one can make it, and the line
    // above is what is checked.
    if (wait_us < 50'000) {
      QVERIFY2(moving >= 12.0, qPrintable(why()));
    } else {
      qWarning("timed waits here overrun (a 40 ms wait took %lld us): %s", static_cast<long long>(wait_us),
               qPrintable(why()));
    }
    QVERIFY(in_order);
    test::settle(*bridge_);
    // nothing is looked for in it: the stage sat on a hole, and no frame says so
    QCOMPARE(with_target, 0);
    bridge_->go_to(QStringLiteral("5"), false);
    test::settle(*bridge_);
    QTest::qWait(300);
    QCOMPARE(with_target, 0);
  }

  void publishes_snapshots_and_views() {
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
  std::unique_ptr<test::SimLaserLab> lab_;
  std::unique_ptr<LaserBridge> bridge_;
  std::unique_ptr<Heard> heard_;
  std::atomic<int> aborts_{0};
};

QTEST_MAIN(LaserBridgeTest)
#include "test_laser_bridge.moc"
