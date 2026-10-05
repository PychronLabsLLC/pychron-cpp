#include "laser_bridge.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

#include <QMetaObject>

namespace pychron::ui {

namespace {

using experiment::lab::Lasers;
using extraction::IStage;

Unexpected<Error> cancelled() { return fail(ErrorKind::Cancelled, "stopped", "laser"); }

laser::LaserSystem& system_of(const LaserBridgeDeps& deps) {
  laser::LaserSystem* system = deps.lasers.find(deps.device);
  if (system == nullptr) throw std::invalid_argument("no extraction device '" + deps.device + "'");
  return *system;
}

}  // namespace

// The worker reaches the bridge only through this, which the destructor
// closes: posting a queued call is thread-safe and never runs the bridge's
// code on the worker.
struct LaserBridge::Gate {
  std::mutex mutex;
  LaserBridge* target = nullptr;
};

struct LaserBridge::Worker {
  struct Command {
    QString what;
    Action action;
    bool driving = true;
  };
  std::mutex mutex;
  std::condition_variable wake;
  std::condition_variable idle;
  std::deque<Command> queue;
  bool busy = false;  // a command is running
  bool quit = false;
  std::atomic<bool> cancel{false};  // of the command in flight
  std::chrono::steady_clock::time_point published{};
  bool watch_only = false;  // as last published
  bool first = true;
};

// The newest picture waiting for the main thread: a slow repaint drops
// frames, it does not queue them.
struct LaserBridge::Video {
  std::mutex mutex;
  std::condition_variable wake;
  bool quit = false;
  std::optional<laser::CameraView> pending;
  bool posted = false;
};

template <class F>
void LaserBridge::post(F&& f) {
  std::lock_guard lock(gate_->mutex);
  if (LaserBridge* self = gate_->target) {
    QMetaObject::invokeMethod(self, std::forward<F>(f), Qt::QueuedConnection);
  }
}

LaserBridge::LaserBridge(LaserBridgeDeps deps, QObject* parent)
    : QObject(parent),
      deps_(std::move(deps)),
      system_(system_of(deps_)),
      gate_(std::make_shared<Gate>()),
      worker_(std::make_unique<Worker>()) {
  gate_->target = this;
  // What the window shows before the first poll comes back.
  state_ = system_.snapshot();
  watch_only_ = deps_.lasers.driver() == Lasers::Driver::Queue;
  worker_->watch_only = watch_only_;
  thread_ = std::thread([this] { loop(); });
  video_ = std::make_unique<Video>();
  if (system_.has_camera()) video_thread_ = std::thread([this] { show(); });
}

void LaserBridge::show() {
  Video& v = *video_;
  std::optional<vision::Target> target;       // what the finder last made out
  std::vector<vision::Target> targets;
  auto found_at = std::chrono::steady_clock::time_point{};
  std::uint64_t shown_seq = 0;
  std::string shown_trouble, failed;
  int shown_age_s = -1;
  for (;;) {
    const auto began = std::chrono::steady_clock::now();
    const bool look = began - found_at >= deps_.find_every;
    auto seen = look ? system_.view() : system_.picture();
    if (seen) {
      failed.clear();
      if (look) {
        target = seen->target;
        targets = seen->targets;
        found_at = began;
      } else {
        seen->target = target;
        seen->targets = targets;
      }
      // A camera that has stopped gives the same frame again: shown again
      // only when what is said about it changes.
      const int age_s = seen->trouble.empty() ? -1 : seen->age_ms / 1000;
      if (seen->frame.seq != shown_seq || seen->trouble != shown_trouble || age_s != shown_age_s || look) {
        shown_seq = seen->frame.seq;
        shown_trouble = seen->trouble;
        shown_age_s = age_s;
        bool post_it = false;
        {
          std::lock_guard lock(v.mutex);
          v.pending = std::move(*seen);
          post_it = !std::exchange(v.posted, true);
        }
        if (post_it) post([this] { deliver_view(); });
      }
    } else if (seen.error().what != failed) {
      failed = seen.error().what;
      post([this, why = QString::fromStdString(failed)] { emit viewFailed(why); });
    }
    std::unique_lock lock(v.mutex);
    if (v.wake.wait_until(lock, began + deps_.video, [&v] { return v.quit; })) return;
  }
}

void LaserBridge::deliver_view() {
  std::optional<laser::CameraView> seen;
  {
    std::lock_guard lock(video_->mutex);
    seen = std::move(video_->pending);
    video_->pending.reset();
    video_->posted = false;
  }
  if (seen) emit view(*seen);
}

LaserBridge::~LaserBridge() {
  {
    std::lock_guard lock(gate_->mutex);
    gate_->target = nullptr;
  }
  {
    std::lock_guard lock(worker_->mutex);
    worker_->quit = true;
    worker_->queue.clear();
    worker_->cancel.store(true);  // a motion being waited for is stopped
  }
  worker_->wake.notify_all();
  {
    std::lock_guard lock(video_->mutex);
    video_->quit = true;
  }
  video_->wake.notify_all();
  if (video_thread_.joinable()) video_thread_.join();
  if (thread_.joinable()) thread_.join();
  // Nobody is left to close a beam this bridge's window opened. A queue's
  // beam is the queue's: its own end closes it.
  if (auto hand = deps_.lasers.drive(Lasers::Driver::Manual)) {
    if (system_.laser() != nullptr && system_.is_firing().value_or(true)) {
      (void)system_.end_extract();
      (void)system_.disable();
    }
  }
}

QStringList LaserBridge::trays() const {
  QStringList out;
  for (const auto& name : deps_.lab.trays.names()) out.push_back(QString::fromStdString(name));
  return out;
}

const laser::TrayMap* LaserBridge::tray_map(const QString& name) const {
  return deps_.lab.trays.find(name.toStdString());
}

bool LaserBridge::has_camera() const { return system_.has_camera(); }

bool LaserBridge::can_center() const { return system_.can_center(); }

std::vector<laser::CalibrationPoint> LaserBridge::calibration_points(const QString& tray) const {
  auto stored = deps_.lab.calibrations->load(deps_.device, tray.toStdString());
  if (!stored || !*stored) return {};
  return (*stored)->points;
}

laser::CalibrationStatus LaserBridge::calibration(const QString& tray) const {
  const laser::TrayMap* map = tray_map(tray);
  if (map == nullptr) return {};
  return deps_.lab.calibrations->status(*map, deps_.device);
}

QStringList LaserBridge::corrected_holes() const {
  QStringList out;
  for (const auto& [hole, correction] : system_.corrections()) out.push_back(QString::fromStdString(hole));
  return out;
}

void LaserBridge::submit(const char* what, Action action, bool driving) {
  {
    std::lock_guard lock(worker_->mutex);
    worker_->queue.push_back({QString::fromLatin1(what), std::move(action), driving});
  }
  worker_->wake.notify_all();
}

void LaserBridge::interrupt(const char* what, Action action, bool driving) {
  std::deque<Worker::Command> dropped;
  {
    std::lock_guard lock(worker_->mutex);
    dropped.swap(worker_->queue);
    if (worker_->busy) worker_->cancel.store(true);
    worker_->queue.push_back({QString::fromLatin1(what), std::move(action), driving});
  }
  worker_->wake.notify_all();
  // Each command reports once, the dropped ones too; after this call returns,
  // as every other result.
  for (auto& command : dropped) {
    QMetaObject::invokeMethod(
        this, [this, name = command.what] { emit commandFinished(name, Result<void>(cancelled())); },
        Qt::QueuedConnection);
  }
}

void LaserBridge::drain() {
  std::unique_lock lock(worker_->mutex);
  worker_->idle.wait(lock, [this] { return worker_->queue.empty() && !worker_->busy; });
}

void LaserBridge::on_snapshot(const laser::LaserSnapshot& state, bool watch_only) {
  state_ = state;
  const bool changed = watch_only != watch_only_;
  watch_only_ = watch_only;
  if (changed) emit driverChanged(watch_only_);
  emit snapshot(state_);
}

void LaserBridge::publish_now() {
  worker_->published = std::chrono::steady_clock::now();
  laser::LaserSnapshot state = system_.snapshot();
  const bool watch_only = deps_.lasers.driver() == Lasers::Driver::Queue;
  post([this, state = std::move(state), watch_only] { on_snapshot(state, watch_only); });
}

void LaserBridge::loop() {
  Worker& w = *worker_;
  for (;;) {
    Worker::Command command;
    bool have = false;
    {
      std::unique_lock lock(w.mutex);
      w.wake.wait_until(lock, w.published + deps_.publish, [&w] { return w.quit || !w.queue.empty(); });
      if (w.quit) return;
      if (!w.queue.empty()) {
        command = std::move(w.queue.front());
        w.queue.pop_front();
        w.busy = true;
        w.cancel.store(false);  // a stop cancels what was running when it was asked for
        have = true;
      }
    }
    if (have) {
      Result<void> result;
      Lasers::Lease lease;
      if (command.driving) {
        auto taken = deps_.lasers.drive(Lasers::Driver::Manual);
        if (taken) lease = std::move(*taken);
        else result = fail(taken.error());
      }
      if (result) result = command.action();
      lease.release();
      post([this, what = command.what, result] { emit commandFinished(what, result); });
      publish_now();  // what it changed is shown at once
      {
        std::lock_guard lock(w.mutex);
        w.busy = false;
      }
      w.idle.notify_all();
    } else if (std::chrono::steady_clock::now() - w.published >= deps_.publish) {
      publish_now();
    }
  }
}

Result<void> LaserBridge::wait(const std::function<Result<bool>()>& busy, const std::function<void()>& halt) {
  Worker& w = *worker_;
  for (;;) {
    if (w.cancel.load()) {
      halt();
      return cancelled();
    }
    auto still = busy();
    if (!still) return fail(std::move(still).error());
    if (!*still) return {};
    if (std::chrono::steady_clock::now() - w.published >= deps_.publish) publish_now();
    std::unique_lock lock(w.mutex);
    w.wake.wait_for(lock, deps_.poll, [&w] { return w.quit || w.cancel.load(); });
  }
}

void LaserBridge::go_to(const QString& hole, bool autocenter) {
  submit("go_to", [this, hole = hole.toStdString(), autocenter]() -> Result<void> {
    if (auto started = system_.move_to_position(hole, autocenter); !started) return started;
    return wait([this] { return system_.moving(); }, [this] { (void)system_.stop(); });
  });
}

void LaserBridge::jog(double dx, double dy, double dz) {
  submit("jog", [this, dx, dy, dz]() -> Result<void> {
    auto at = system_.position();
    if (!at) return fail(std::move(at).error());
    const auto arrive = [this] {
      return wait([this] { return system_.moving(); }, [this] { (void)system_.stop(); });
    };
    if (dx != 0 || dy != 0) {
      if (auto started = system_.set_xy(at->x + dx, at->y + dy); !started) return started;
      if (auto arrived = arrive(); !arrived) return arrived;
    }
    if (dz != 0) {
      if (auto started = system_.set_axis(IStage::Axis::Z, at->z + dz); !started) return started;
      if (auto arrived = arrive(); !arrived) return arrived;
    }
    return {};
  });
}

void LaserBridge::stop_stage() {
  interrupt("stop_stage", [this] { return system_.stop(); }, true);
}

void LaserBridge::set_tray(const QString& tray) {
  submit("set_tray", [this, tray = tray.toStdString()]() -> Result<void> {
    auto set = system_.set_tray(tray);
    if (set) post([this] { emit calibrationChanged(); });
    return set;
  });
}

void LaserBridge::enable(bool on) {
  submit("enable", [this, on] { return on ? system_.enable() : system_.disable(); });
}

void LaserBridge::fire(double percent) {
  submit("fire", [this, percent]() -> Result<void> {
    auto* laser = system_.laser();
    if (laser == nullptr) return fail(ErrorKind::Config, "the device has no laser", deps_.device);
    if (auto set = system_.extract(percent, extraction::ExtractUnits::Percent); !set) return set;
    auto fired = laser->fire_laser();
    // An output with no beam is not left standing.
    if (!fired) (void)system_.end_extract();
    return fired;
  });
}

void LaserBridge::set_output(double percent) {
  submit("set_output", [this, percent] { return system_.extract(percent, extraction::ExtractUnits::Percent); });
}

void LaserBridge::stop_beam() {
  interrupt("stop_beam", [this] { return system_.end_extract(); }, true);
}

void LaserBridge::run_pattern(const QString& name) {
  submit("run_pattern", [this, name = name.toStdString()]() -> Result<void> {
    auto* runner = system_.pattern_runner();
    if (runner == nullptr) return fail(ErrorKind::Config, "the device runs no patterns", deps_.device);
    if (auto started = runner->execute_pattern(name); !started) return started;
    return wait([runner] { return runner->running(); }, [runner] { (void)runner->stop_pattern(); });
  });
}

void LaserBridge::stop_pattern() {
  interrupt(
      "stop_pattern",
      [this]() -> Result<void> {
        auto* runner = system_.pattern_runner();
        return runner != nullptr ? runner->stop_pattern() : Result<void>{};
      },
      true);
}

Result<void> LaserBridge::change_calibration(
    const std::function<void(std::vector<laser::CalibrationPoint>&, double, double)>& edit) {
  const std::string tray = system_.tray();
  const laser::TrayMap* map = deps_.lab.trays.find(tray);
  if (map == nullptr) return fail(ErrorKind::Config, "no tray is set", deps_.device);
  auto at = system_.position();
  if (!at) return fail(std::move(at).error());
  const laser::CalibrationStore& store = *deps_.lab.calibrations;
  auto stored = store.load(deps_.device, tray);
  if (!stored) return fail(std::move(stored).error());
  std::vector<laser::CalibrationPoint> points;
  // Points taken against another version of the map are not built on.
  if (*stored && (*stored)->tray_sha256 == map->sha256()) points = (*stored)->points;
  edit(points, at->x, at->y);
  auto saved = points.empty() ? store.clear(deps_.device, tray) : store.save(*map, deps_.device, points);
  if (!saved) return saved;
  // The system reads a tray's calibration when the tray is set.
  auto reread = system_.set_tray(tray);
  post([this] { emit calibrationChanged(); });
  return reread;
}

void LaserBridge::add_calibration_point(const QString& hole) {
  submit("add_calibration_point", [this, hole = hole.toStdString()] {
    return change_calibration([&hole](std::vector<laser::CalibrationPoint>& points, double x, double y) {
      std::erase_if(points, [&hole](const laser::CalibrationPoint& p) { return p.hole == hole; });
      points.push_back({hole, x, y});
    });
  });
}

void LaserBridge::remove_calibration_point(const QString& hole) {
  submit("remove_calibration_point", [this, hole = hole.toStdString()] {
    return change_calibration([&hole](std::vector<laser::CalibrationPoint>& points, double, double) {
      std::erase_if(points, [&hole](const laser::CalibrationPoint& p) { return p.hole == hole; });
    });
  });
}

void LaserBridge::clear_calibration() {
  submit("clear_calibration", [this] {
    return change_calibration([](std::vector<laser::CalibrationPoint>& points, double, double) { points.clear(); });
  });
}

void LaserBridge::snapshot_to_file() {
  submit(
      "snapshot",
      [this]() -> Result<void> {
        auto* imaging = system_.imaging();
        if (imaging == nullptr) return fail(ErrorKind::Config, "no camera", deps_.device);
        auto saved = imaging->snapshot({});
        if (!saved) return fail(std::move(saved).error());
        post([this, file = QString::fromStdString(*saved)] { emit snapshotSaved(file); });
        return {};
      },
      false);
}

void LaserBridge::measure_scale(double step_mm) {
  submit("measure_scale", [this, step_mm]() -> Result<void> {
    const laser::CameraConfig* camera = deps_.lab.cameras.find(deps_.device);
    const auto settle = std::chrono::duration_cast<std::chrono::milliseconds>(
        camera != nullptr ? camera->settle : Duration(std::chrono::milliseconds(200)));
    // The stage at rest, then the picture given time to settle (real time:
    // the camera's, not a simulated clock's).
    const auto arrive = [this, settle]() -> Result<void> {
      if (auto at_rest = wait([this] { return system_.moving(); }, [this] { (void)system_.stop(); }); !at_rest) {
        return at_rest;
      }
      const auto until = std::chrono::steady_clock::now() + settle;
      return wait([until]() -> Result<bool> { return std::chrono::steady_clock::now() < until; }, [] {});
    };
    auto measured = laser::measure_camera_scale(system_, step_mm, arrive);
    if (!measured) return fail(std::move(measured).error());
    if (auto saved = deps_.lab.camera_scales->save(deps_.device, *measured); !saved) return saved;
    system_.set_measured_scale(*measured);
    post([this, m = *measured] { emit scaleMeasured(m); });
    return {};
  });
}

void LaserBridge::emergency_stop() {
  // At once, here: the worker may be waiting for the device, or for a camera.
  system_.latch_stop();
  interrupt(
      "emergency_stop",
      [this]() -> Result<void> {
        auto stopped = system_.emergency_stop();
        if (deps_.abort_queue) deps_.abort_queue();
        return stopped;
      },
      false);
}

void LaserBridge::reset_stop() {
  submit(
      "reset_stop",
      [this]() -> Result<void> {
        // A queue that was aborted may not have seen it yet: its script's
        // next call must still be refused.
        if (deps_.lasers.driver() == Lasers::Driver::Queue) {
          return fail(ErrorKind::Config, "a queue is still running: reset the stop once it has ended", deps_.device);
        }
        system_.reset_stop();
        return {};
      },
      false);
}

}  // namespace pychron::ui
