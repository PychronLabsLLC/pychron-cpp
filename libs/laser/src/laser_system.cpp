#include "pychron/laser/laser_system.hpp"

#include "pychron/laser/pattern_runner.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <utility>

#include "pychron/vision/finder.hpp"
#include "pychron/vision/live_feed.hpp"
#include "pychron/vision/png.hpp"

namespace pychron::laser {

using extraction::IStage;
using extraction::StagePosition;

namespace {

const Clock& default_clock() {
  static const SteadyClock clock;
  return clock;
}

}  // namespace

LaserSystem::LaserSystem(std::string name, extraction::IExtractionDevice& driver, const TrayLibrary& trays,
                         const CalibrationStore& calibrations, const PatternLibrary* patterns, const Clock* clock)
    : name_(std::move(name)),
      driver_(driver),
      trays_(trays),
      calibrations_(calibrations),
      gate_(clock != nullptr ? *clock : default_clock()),
      gate_clock_(clock) {
  // The runner moves this system's stage, not the driver's directly, so
  // whatever the system adds to a move applies to a pattern's too.
  if (patterns != nullptr) runner_ = std::make_unique<PatternRunner>(name_, *this, *patterns);
}

namespace {

using Gate = std::lock_guard<RecursiveClockMutex>;

// Calendar time for a stamp that is written down: the clock's, so a simulated
// session is stamped in simulated time.
std::tm utc_parts(const Clock& clock) {
  const std::time_t now = std::chrono::system_clock::to_time_t(clock.wall_now());
  std::tm parts{};
#ifdef _WIN32
  gmtime_s(&parts, &now);
#else
  gmtime_r(&now, &parts);
#endif
  return parts;
}

}  // namespace

std::string_view to_string(LaserActivity activity) noexcept {
  switch (activity) {
    case LaserActivity::Idle: return "idle";
    case LaserActivity::Moving: return "moving";
    case LaserActivity::Centering: return "centering";
    case LaserActivity::Pattern: return "pattern";
  }
  return "idle";
}

// Whichever runner the device has, with each call behind the system's gate
// and the emergency stop's latch in front of a start.
class LaserSystem::GatedRunner final : public extraction::IPatternRunner {
 public:
  explicit GatedRunner(LaserSystem& system) : system_(system) {}

  Result<void> execute_pattern(std::string_view pattern) override { return execute_pattern_for(pattern, 0); }
  Result<void> execute_pattern_for(std::string_view pattern, double duration_s) override {
    Gate gate(system_.gate_);
    if (auto ok = system_.allowed(); !ok) return ok;
    return system_.inner_runner()->execute_pattern_for(pattern, duration_s);
  }
  Result<bool> running() override {
    Gate gate(system_.gate_);
    return system_.inner_runner()->running();
  }
  Result<void> stop_pattern() override {
    Gate gate(system_.gate_);
    return system_.inner_runner()->stop_pattern();
  }
  std::vector<std::string> patterns() const override { return system_.inner_runner()->patterns(); }
  bool needs_polling() const override { return system_.inner_runner()->needs_polling(); }
  std::string last_note() override {
    Gate gate(system_.gate_);
    return system_.inner_runner()->last_note();
  }

 private:
  LaserSystem& system_;
};

LaserSystem::~LaserSystem() = default;

extraction::IPatternRunner* LaserSystem::inner_runner() {
  if (auto* own = driver_.pattern_runner()) return own;
  return driver_.stage() != nullptr ? runner_.get() : nullptr;
}

extraction::IPatternRunner* LaserSystem::pattern_runner() {
  if (inner_runner() == nullptr) return nullptr;
  Gate gate(gate_);
  if (gated_runner_ == nullptr) gated_runner_ = std::make_unique<GatedRunner>(*this);
  return gated_runner_.get();
}

Result<void> LaserSystem::allowed() const {
  if (!stopped_.load()) return {};
  return fail(ErrorKind::Interlock, "emergency stop: reset it in the laser window", name_);
}

Result<void> LaserSystem::prepare() {
  Gate gate(gate_);
  return driver_.prepare();
}

Result<void> LaserSystem::enable() {
  Gate gate(gate_);
  if (auto ok = allowed(); !ok) return ok;
  return driver_.enable();
}

Result<void> LaserSystem::disable() {
  Gate gate(gate_);
  return driver_.disable();
}

Result<bool> LaserSystem::is_enabled() {
  Gate gate(gate_);
  return driver_.is_enabled();
}

Result<void> LaserSystem::extract(double value, extraction::ExtractUnits units) {
  Gate gate(gate_);
  if (auto ok = allowed(); !ok) return ok;
  return driver_.extract(value, units);
}

Result<void> LaserSystem::end_extract() {
  Gate gate(gate_);
  return driver_.end_extract();
}

Result<double> LaserSystem::output() {
  Gate gate(gate_);
  return driver_.output();
}

namespace {

Unexpected<Error> no_laser(const std::string& name) { return fail(ErrorKind::Config, "the device has no laser", name); }

}  // namespace

Result<void> LaserSystem::fire_laser() {
  Gate gate(gate_);
  auto* laser = driver_.laser();
  if (laser == nullptr) return no_laser(name_);
  if (auto ok = allowed(); !ok) return ok;
  return laser->fire_laser();
}

Result<void> LaserSystem::stop_laser() {
  Gate gate(gate_);
  auto* laser = driver_.laser();
  if (laser == nullptr) return no_laser(name_);
  return laser->stop_laser();
}

Result<bool> LaserSystem::is_firing() {
  Gate gate(gate_);
  auto* laser = driver_.laser();
  if (laser == nullptr) return no_laser(name_);
  return laser->is_firing();
}

Result<void> LaserSystem::warmup() {
  Gate gate(gate_);
  auto* laser = driver_.laser();
  if (laser == nullptr) return no_laser(name_);
  if (auto ok = allowed(); !ok) return ok;
  return laser->warmup();
}

Result<std::vector<std::string>> LaserSystem::tripped_interlocks() {
  Gate gate(gate_);
  auto* laser = driver_.laser();
  if (laser == nullptr) return no_laser(name_);
  return laser->tripped_interlocks();
}

LaserSnapshot LaserSystem::snapshot() {
  LaserSnapshot s;
  s.device = name_;
  {
    Gate gate(gate_);
    const auto note = [&s](const Error& e) {
      if (s.error.empty()) s.error = e.what;
    };
    IStage* stage = driver_.stage();
    auto* laser = driver_.laser();
    s.has_stage = stage != nullptr;
    s.has_laser = laser != nullptr;
    s.has_camera = frames_ != nullptr;
    if (stage != nullptr) {
      if (auto at = stage->position()) s.position = *at;
      else note(at.error());
    }
    if (auto on = driver_.is_enabled()) s.enabled = *on;
    else note(on.error());
    if (auto out = driver_.output()) s.output = *out;
    else note(out.error());
    if (laser != nullptr) {
      if (auto firing = laser->is_firing()) s.firing = *firing;
      else note(firing.error());
      if (auto tripped = laser->tripped_interlocks()) s.interlocks = std::move(*tripped);
      else note(tripped.error());
    }
    if (runner_ != nullptr && driver_.pattern_runner() == nullptr) s.pattern_progress = runner_->progress();
    s.activity = !s.pattern_progress.empty() ? LaserActivity::Pattern
                 : centering_ != nullptr     ? LaserActivity::Centering
                 : moving_.load()            ? LaserActivity::Moving
                                             : LaserActivity::Idle;
    s.stopped = stopped_.load();
  }
  std::lock_guard lock(mutex_);
  if (tray_ != nullptr) s.tray = tray_->name();
  s.calibration = status_.state;
  s.calibration_why = status_.why;
  s.last_hole = last_hole_;
  s.autocenter = outcome_;
  return s;
}

extraction::IImaging* LaserSystem::imaging() {
  if (auto* own = driver_.imaging()) return own;
  return has_camera() ? this : nullptr;
}

void LaserSystem::set_snapshot_dir(std::filesystem::path dir) {
  std::lock_guard lock(mutex_);
  snapshot_dir_ = std::move(dir);
}

Result<std::string> LaserSystem::snapshot(std::string_view name) {
  namespace fs = std::filesystem;
  fs::path dir;
  {
    std::lock_guard lock(mutex_);
    dir = snapshot_dir_;
  }
  if (dir.empty()) return fail(ErrorKind::Config, "no directory to keep snapshots in", name_);
  std::string stem(name);
  if (stem.ends_with(".png")) stem.erase(stem.size() - 4);
  if (stem.empty()) {
    const Clock* clock = nullptr;
    {
      Gate gate(gate_);  // attach_camera() sets both
      if (frames_ == nullptr) return fail(ErrorKind::Config, "no camera", name_);
      clock = clock_;
    }
    const std::tm parts = utc_parts(*clock);
    char text[32];
    std::strftime(text, sizeof text, "%Y%m%d-%H%M%S", &parts);
    stem = text;
  }
  // One plain part of a file name: a snapshot stays in its directory.
  if (!safe_file_part(stem) || stem.front() == '.' || stem.find(':') != std::string::npos) {
    return fail(ErrorKind::Config, "'" + std::string(name) + "' cannot name a snapshot (it names the file)", name_);
  }
  auto seen = view();
  if (!seen) return fail(std::move(seen).error());
  if (!seen->trouble.empty()) {
    return fail(ErrorKind::Io, "the camera has stopped (" + seen->trouble + "): its last picture is not a snapshot of now", name_);
  }
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) return fail(ErrorKind::Io, "cannot make " + dir.string() + ": " + ec.message(), name_);
  fs::path file = dir / (stem + ".png");
  for (int n = 2; fs::exists(file, ec); ++n) file = dir / (stem + "-" + std::to_string(n) + ".png");
  if (auto written = vision::write_png(file, seen->frame.view()); !written) return fail(std::move(written).error());
  return file.string();
}

Result<void> LaserSystem::start_video_recording(std::string_view) {
  return fail(extraction::not_supported("video recording", name_));
}

Result<void> LaserSystem::stop_video_recording() { return fail(extraction::not_supported("video recording", name_)); }

std::string LaserSystem::camera_geometry() const {
  Gate gate(gate_);
  return camera_ ? camera_->geometry() : std::string{};
}

void LaserSystem::set_measured_scale(const ScaleMeasurement& measured) {
  Gate gate(gate_);
  if (camera_) camera_->measured = measured.map;
}

Result<CameraView> LaserSystem::frame_now(bool fresh) {
  if (frames_ == nullptr) return fail(ErrorKind::Config, "no camera", name_);
  CameraView seen;
  if (auto* live = fresh ? nullptr : dynamic_cast<vision::LiveFeed*>(frames_.get())) {
    // Never waited for: the newest picture there is, and how it is doing.
    vision::LiveFeed::Latest latest = live->latest();
    if (!latest.frame) {
      return fail(ErrorKind::Io, latest.error.empty() ? "no picture from the camera yet" : latest.error, name_);
    }
    seen.frame = std::move(*latest.frame);
    seen.age_ms = static_cast<int>(latest.age.count());
    seen.fps = latest.fps;
    seen.trouble = std::move(latest.error);
  } else {
    auto frame = frames_->grab();
    if (!frame) return fail(std::move(frame).error());
    seen.frame = std::move(*frame);
  }
  seen.px_per_mm = camera_->scale_px_per_mm();
  seen.aim_px = {(seen.frame.width - 1) / 2.0 + camera_->aim_offset_px.x,
                 (seen.frame.height - 1) / 2.0 + camera_->aim_offset_px.y};
  double radius_mm = 0.5;
  {
    std::lock_guard lock(mutex_);
    if (tray_ != nullptr && tray_->dimension() > 0) radius_mm = tray_->dimension() / 2;
  }
  seen.expected_radius_px = radius_mm * seen.px_per_mm;
  return seen;
}

bool LaserSystem::looking() {
  Gate gate(gate_);
  if (centering_ != nullptr) return true;
  return runner_ != nullptr && driver_.pattern_runner() == nullptr && runner_->following();
}

Result<CameraView> LaserSystem::picture() {
  Gate gate(gate_);
  return frame_now(false);
}

Result<CameraView> LaserSystem::view(bool fresh, bool any_size) {
  CameraView seen;
  bool firing = false;
  {
    Gate gate(gate_);
    auto frame = frame_now(fresh);
    if (!frame) return frame;
    seen = std::move(*frame);
    if (auto* laser = driver_.laser()) firing = laser->is_firing().value_or(false);
  }
  // The looking is done on a frame of the caller's own, outside the gate: a
  // big picture holds up neither the device nor a stop.
  // A hole, dark on the tray; with the beam on, the sample's glow.
  vision::FinderParams params;
  params.mode = firing ? vision::FinderMode::Glow : vision::FinderMode::Hole;
  params.expected_radius_px = any_size ? 0.0 : seen.expected_radius_px;
  vision::SimpleFinder finder;
  if (any_size) {
    seen.targets = finder.find(seen.frame.view(), params);
  } else {
    // About the aim point only: what a centering would go for is there, and
    // a camera's whole frame is far more than that.
    const double side = std::clamp(8.0 * seen.expected_radius_px, 160.0, 1.0e6);
    const int x0 = std::max(0, static_cast<int>(std::lround(seen.aim_px.x - side / 2)));
    const int y0 = std::max(0, static_cast<int>(std::lround(seen.aim_px.y - side / 2)));
    const int x1 = std::min(seen.frame.width, static_cast<int>(std::lround(seen.aim_px.x + side / 2)));
    const int y1 = std::min(seen.frame.height, static_cast<int>(std::lround(seen.aim_px.y + side / 2)));
    if (x1 > x0 && y1 > y0) {
      const vision::Frame part = vision::crop(seen.frame.view(), vision::Rect{x0, y0, x1 - x0, y1 - y0});
      seen.targets = finder.find(part.view(), params);
      for (auto& target : seen.targets) {
        target.center_px.x += x0;
        target.center_px.y += y0;
      }
    }
  }
  // The one nearest the aim: what a centering would go for.
  for (const auto& target : seen.targets) {
    const auto off = [&seen](const vision::Target& t) {
      return std::hypot(t.center_px.x - seen.aim_px.x, t.center_px.y - seen.aim_px.y);
    };
    if (!seen.target || off(target) < off(*seen.target)) seen.target = target;
  }
  return seen;
}

Result<void> LaserSystem::emergency_stop() {
  // Before the gate: a call that is waiting for it is refused when it gets it.
  stopped_.store(true);
  Gate gate(gate_);
  Result<void> first;
  const auto tried = [&first](Result<void> step) {
    if (!step && first) first = std::move(step);
  };
  if (auto* laser = driver_.laser()) tried(laser->stop_laser());
  tried(driver_.end_extract());
  tried(driver_.disable());
  if (auto* runner = inner_runner()) tried(runner->stop_pattern());
  if (IStage* stage = driver_.stage()) {
    // A stage that cannot be stopped finishes its move; there is no more to do.
    if (auto halted = stage->stop(); !halted && !extraction::is_not_supported(halted.error())) {
      tried(std::move(halted));
    }
  }
  abandon(AutocenterOutcome::Result::Stopped);
  moving_.store(false);
  return first;
}

std::string LaserSystem::tray() const {
  std::lock_guard lock(mutex_);
  return tray_ != nullptr ? tray_->name() : std::string{};
}

CalibrationStatus LaserSystem::calibration() const {
  std::lock_guard lock(mutex_);
  return status_;
}

Result<IStage*> LaserSystem::driver_stage() {
  IStage* stage = driver_.stage();
  if (stage == nullptr) return fail(ErrorKind::Config, "the device has no stage", name_);
  return stage;
}

// An autocenter in progress: the stage is travelling to the hole, at rest
// and being looked at, or on its way back after a failure.
struct LaserSystem::Centering {
  enum class Phase { Center, Return } phase = Phase::Center;
  std::string hole;
  std::string tray;
  StageXY nominal{};  // where the calibration puts the hole
  StageXY start{};    // where the centering starts: the correction, or nominal
  double guard = 0;
  vision::Autocenter controller;
  std::optional<TimePoint> at_rest;  // since when the stage has been seen stopped
  int looks = 0;
  int stale = 0;
  double residual = 0;
  vision::AutocenterReason failed = vision::AutocenterReason::None;

  Centering(vision::ITargetFinder& finder, const CameraConfig& camera, vision::AutocenterParams params)
      : controller(finder, camera.map(), camera.scale_px_per_mm(), params) {}
};

namespace {

// How many batches of frames no newer than the last decision are waited out
// before a camera is taken to have stopped.
constexpr int kStaleTries = 5;

double apart(StageXY a, StageXY b) { return std::hypot(a.x - b.x, a.y - b.y); }

std::string utc_now(const Clock& clock) {
  const std::tm parts = utc_parts(clock);
  char text[32];
  std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%SZ", &parts);
  return text;
}

}  // namespace

void LaserSystem::set_corrections(const CorrectionStore& corrections) { correction_store_ = &corrections; }

// The gate is waited for on the system's clock and the camera's settle and
// stamps are read from the camera's: with two, a wait in simulated time would
// be measured against real time. A system given no clock is on real time, and
// one SteadyClock is as good as another.
Result<void> LaserSystem::one_clock(const Clock& clock) const {
  if (gate_clock_ == &clock) return {};
  if (gate_clock_ == nullptr && dynamic_cast<const SteadyClock*>(&clock) != nullptr) return {};
  return fail(ErrorKind::Config,
              "two clocks in " + name_ + ": its camera is on another clock than the one the system was " +
                  (gate_clock_ != nullptr ? "given" : "left on (real time, none given)"),
              name_);
}

Result<void> LaserSystem::attach_camera(CameraConfig config, std::unique_ptr<vision::IFrameSource> frames,
                                        const Clock& clock) {
  Gate gate(gate_);
  if (auto same = one_clock(clock); !same) return same;
  // Whether the stage and the camera suit each other is the caller's to
  // know; that a recording, or a camera for looking, never moves a stage is
  // known here.
  if (config.use == CameraUse::View || config.source == CameraSource::Recorded) {
    return usable_for_autocenter(config, config.source == CameraSource::Sim);
  }
  if (frames == nullptr) return fail(ErrorKind::Config, "the camera of " + name_ + " has no frames", name_);
  camera_ = std::move(config);
  frames_ = std::move(frames);
  centers_ = true;
  clock_ = &clock;
  finder_ = std::make_unique<vision::SimpleFinder>();
  // The same eyes for a pattern that follows the glow.
  if (runner_ != nullptr) {
    runner_->set_vision({frames_.get(), &*camera_, clock_, [this] { return hole_room_; }});
  }
  return {};
}

Result<void> LaserSystem::attach_viewer(CameraConfig config, std::unique_ptr<vision::IFrameSource> frames,
                                        const Clock& clock) {
  Gate gate(gate_);
  if (auto same = one_clock(clock); !same) return same;
  if (frames == nullptr) return fail(ErrorKind::Config, "the camera of " + name_ + " has no frames", name_);
  camera_ = std::move(config);
  frames_ = std::move(frames);
  centers_ = false;
  clock_ = &clock;
  finder_ = std::make_unique<vision::SimpleFinder>();
  if (runner_ != nullptr) runner_->set_vision({});  // nothing to follow the glow with
  return {};
}

std::string LaserSystem::last_move_note() {
  std::lock_guard lock(mutex_);
  return std::exchange(move_note_, {});
}

namespace {

std::string mm(double value) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::fixed << std::setprecision(3) << value;
  std::string text = out.str();
  if (text.starts_with('-') && text.find_first_not_of("-0.") == std::string::npos) text.erase(0, 1);
  return text;
}

}  // namespace

AutocenterOutcome LaserSystem::last_autocenter() const {
  std::lock_guard lock(mutex_);
  return outcome_;
}

HoleCorrections LaserSystem::corrections() const {
  std::lock_guard lock(mutex_);
  return corrections_;
}

double LaserSystem::guard_locked(const Hole& hole) const {
  double nearest = std::numeric_limits<double>::infinity();
  for (const auto& other : tray_->holes()) {
    if (&other == &hole) continue;
    nearest = std::min(nearest, std::hypot(other.x - hole.x, other.y - hole.y));
  }
  return std::min(1.0, 0.45 * nearest);  // a tray of one hole: the cap
}

double LaserSystem::guard_mm(std::string_view hole) const {
  std::lock_guard lock(mutex_);
  if (tray_ == nullptr) return 0;
  const Hole* found = tray_->find(hole);
  return found == nullptr ? 0 : guard_locked(*found);
}

TraySightFn LaserSystem::sight() {
  return [this]() {
    Gate gate(gate_);
    TraySight seen;
    if (IStage* stage = driver_.stage()) {
      if (auto at = stage->position()) seen.stage = {at->x, at->y};
    }
    if (auto* laser = driver_.laser()) {
      seen.firing = laser->is_firing().value_or(false);
      if (seen.firing) seen.output_percent = driver_.output().value_or(0);
    }
    std::lock_guard lock(mutex_);
    if (tray_ != nullptr && status_.solution) {
      seen.hole_radius_mm = tray_->dimension() / 2;
      for (const auto& hole : tray_->holes()) {
        seen.holes.push_back(status_.solution->transform.to_stage(hole.x, hole.y));
      }
    }
    return seen;
  };
}

void LaserSystem::abandon(AutocenterOutcome::Result how) {
  if (centering_ == nullptr) return;
  AutocenterOutcome outcome;
  outcome.result = how;
  outcome.hole = centering_->hole;
  outcome.tray = centering_->tray;
  outcome.iterations = centering_->looks;
  centering_.reset();
  std::lock_guard lock(mutex_);
  outcome_ = std::move(outcome);
}

Result<void> LaserSystem::move_to_position(std::string_view position, bool autocenter) {
  Gate gate(gate_);
  auto stage = driver_stage();
  if (!stage) return fail(stage.error());
  if (auto ok = allowed(); !ok) return ok;
  abandon(AutocenterOutcome::Result::Stopped);  // a new move takes over
  hole_room_ = 0;
  {
    std::lock_guard lock(mutex_);
    move_note_.clear();  // of an earlier move nobody asked about
    last_hole_.clear();
  }

  std::string tray;
  std::optional<StageXY> nominal;
  StageXY start{};
  double guard = 0;
  double room = 0;
  double hole_radius = 0.5;
  CalibrationStatus status;
  bool is_hole = false;
  {
    std::lock_guard lock(mutex_);
    if (tray_ != nullptr) {
      tray = tray_->name();
      if (const Hole* hole = tray_->find(position)) {
        is_hole = true;
        status = status_;
        if (status.solution) {
          nominal = status.solution->transform.to_stage(hole->x, hole->y);
          start = *nominal;
          guard = guard_locked(*hole);
          hole_radius = hole->dimension / 2;
          // uncapped, for a pattern: the whole room the hole has
          double nearest = std::numeric_limits<double>::infinity();
          for (const auto& other : tray_->holes()) {
            if (&other != hole) nearest = std::min(nearest, std::hypot(other.x - hole->x, other.y - hole->y));
          }
          room = std::isfinite(nearest) ? 0.45 * nearest : 0.0;
          // Where it was found last time, unless that is further off than
          // any honest correction could be.
          if (const auto it = corrections_.find(position); it != corrections_.end()) {
            const StageXY corrected{it->second.x, it->second.y};
            if (apart(corrected, *nominal) <= guard) start = corrected;
          }
        }
      }
    }
  }

  if (is_hole) {
    if (!nominal) return fail(ErrorKind::Config, status.why, name_);
    auto moved = (*stage)->set_xy(start.x, start.y);
    if (!moved) {
      Error e = std::move(moved).error();
      e.what = "hole " + std::string(position) + " on " + tray + ": " + e.what;
      return fail(std::move(e));
    }
    hole_room_ = room;
    moving_.store(true);
    {
      std::lock_guard lock(mutex_);
      last_hole_ = std::string(position);
    }
    if (autocenter && can_center()) {
      vision::AutocenterParams params;
      params.hole_radius_mm = hole_radius;
      params.tolerance_mm = camera_->tolerance_mm;
      params.max_iterations = camera_->max_iterations;
      params.max_step_mm = camera_->max_step_mm;
      params.max_total_mm = guard;
      params.frames_per_step = camera_->frames_per_step;
      params.aim_offset_px = {camera_->aim_offset_px.x, camera_->aim_offset_px.y};
      centering_ = std::make_unique<Centering>(*finder_, *camera_, params);
      centering_->hole = std::string(position);
      centering_->tray = tray;
      centering_->nominal = *nominal;
      centering_->start = start;
      centering_->guard = guard;
    }
    return {};
  }

  // Not a hole: a position the driver names itself (a Chromium scan).
  auto moved = (*stage)->move_to_position(position, false);
  if (!moved && moved.error().kind == ErrorKind::Config) {
    Error e = std::move(moved).error();
    e.device = name_;
    e.what = (tray.empty() ? "no tray is set, and " : "no hole '" + std::string(position) + "' on tray " + tray + ", and ") +
             e.what;
    return fail(std::move(e));
  }
  if (moved) moving_.store(true);
  return moved;
}

void LaserSystem::off_the_hole() {
  abandon(AutocenterOutcome::Result::Stopped);
  hole_room_ = 0;  // off the hole, for all the system knows
  std::lock_guard lock(mutex_);
  last_hole_.clear();
}

Result<void> LaserSystem::set_axis(Axis axis, double value) {
  Gate gate(gate_);
  auto stage = driver_stage();
  if (!stage) return fail(stage.error());
  if (auto ok = allowed(); !ok) return ok;
  off_the_hole();
  auto moved = (*stage)->set_axis(axis, value);
  if (moved) moving_.store(true);
  return moved;
}

Result<void> LaserSystem::set_xy(double x, double y, double speed_mm_s) {
  Gate gate(gate_);
  auto stage = driver_stage();
  if (!stage) return fail(stage.error());
  if (auto ok = allowed(); !ok) return ok;
  off_the_hole();
  auto moved = (*stage)->set_xy(x, y, speed_mm_s);
  if (moved) moving_.store(true);
  return moved;
}

Result<void> LaserSystem::stop() {
  Gate gate(gate_);
  auto stage = driver_stage();
  if (!stage) return fail(stage.error());
  abandon(AutocenterOutcome::Result::Stopped);
  auto halted = (*stage)->stop();
  if (halted) moving_.store(false);
  return halted;
}

Result<StagePosition> LaserSystem::position() {
  Gate gate(gate_);
  auto stage = driver_stage();
  if (!stage) return fail(stage.error());
  return (*stage)->position();
}

Result<bool> LaserSystem::moving() {
  Gate gate(gate_);
  auto busy = advance();
  // A move that failed is over: whoever asked has been told, and a watcher
  // is not shown a stage moving for ever.
  moving_.store(busy ? *busy : false);
  return busy;
}

Result<bool> LaserSystem::advance() {
  auto stage = driver_stage();
  if (!stage) return fail(stage.error());
  if (centering_ == nullptr) return (*stage)->moving();

  auto busy = (*stage)->moving();
  if (!busy) {
    abandon(AutocenterOutcome::Result::Failed);  // the stage itself failed
    return busy;
  }
  Centering& c = *centering_;
  if (*busy) {
    c.at_rest.reset();
    return true;
  }

  if (c.phase == Centering::Phase::Return) {
    // Back where the centering started: it failed, and now it is over.
    AutocenterOutcome outcome;
    outcome.result = AutocenterOutcome::Result::Failed;
    outcome.reason = c.failed;
    outcome.hole = c.hole;
    outcome.tray = c.tray;
    outcome.iterations = c.looks;
    outcome.found = c.start;
    outcome.residual_mm = c.residual;
    const std::string what = "hole " + c.hole + " on " + c.tray + ": autocenter failed (" +
                             std::string(to_string(c.failed)) + ")";
    const bool corrected = apart(c.start, c.nominal) > 0;
    centering_.reset();
    {
      std::lock_guard lock(mutex_);
      move_note_ = "hole " + outcome.hole + ": not centered (" + std::string(to_string(outcome.reason)) + "); at its " +
                   (corrected ? "last found" : "calibrated") + " position";
      outcome_ = std::move(outcome);
    }
    if (camera_->on_failure == OnAutocenterFailure::Fail) return fail(ErrorKind::Config, what, name_);
    return false;
  }

  // At rest: frames are trusted only once it has been so for the settle time.
  const TimePoint now = clock_->now();
  if (!c.at_rest) c.at_rest = now;
  if (now - *c.at_rest < camera_->settle) return true;
  return look(**stage);
}

Result<bool> LaserSystem::look(IStage& stage) {
  Centering& c = *centering_;
  std::vector<vision::Frame> frames;
  // One wait for the whole look, not one per frame: the device is held
  // meanwhile, and a stop would be held behind it.
  const auto began = std::chrono::steady_clock::now();
  for (int i = 0; i < camera_->frames_per_step; ++i) {
    auto frame = frames_->grab();
    if (!frame) break;
    frames.push_back(std::move(*frame));
    if (camera_->live() && std::chrono::steady_clock::now() - began > camera_->live_timeout) break;
  }
  // The stop was pressed while the camera was waited for: nothing more is
  // sent to the stage, not even the way back.
  if (stopped_.load()) {
    abandon(AutocenterOutcome::Result::Stopped);
    moving_.store(false);
    return false;
  }
  if (static_cast<int>(frames.size()) < camera_->frames_per_step) return give_up(stage, vision::AutocenterReason::Camera);
  std::vector<vision::FrameView> views;
  for (const auto& frame : frames) views.push_back(frame.view());
  const vision::AutocenterStep step = c.controller.step(views);
  using Action = vision::AutocenterStep::Action;

  if (step.action == Action::Failed) {
    // Frames no newer than the last decision: the camera may only be slow.
    // A few polls are given for a newer one, then it has stopped.
    if (step.reason == vision::AutocenterReason::StaleFrame && ++c.stale < kStaleTries) return true;
    c.residual = std::hypot(step.offset_mm.x, step.offset_mm.y);
    return give_up(stage, step.reason);
  }
  c.stale = 0;
  ++c.looks;
  c.residual = std::hypot(step.offset_mm.x, step.offset_mm.y);

  auto at = stage.position();
  if (!at) {
    abandon(AutocenterOutcome::Result::Failed);
    return fail(std::move(at).error());
  }
  const StageXY here{at->x, at->y};

  if (step.action == Action::Converged) {
    // Centered on something: only the hole itself can be this near where the
    // calibration puts it.
    if (apart(here, c.nominal) > c.guard) return give_up(stage, vision::AutocenterReason::MaxTotal);
    AutocenterOutcome outcome;
    outcome.result = AutocenterOutcome::Result::Converged;
    outcome.hole = c.hole;
    outcome.tray = c.tray;
    outcome.iterations = c.looks;
    outcome.found = here;
    outcome.moved_mm = {here.x - c.start.x, here.y - c.start.y};
    outcome.residual_mm = c.residual;
    const HoleCorrection correction{here.x, here.y, c.residual, utc_now(*clock_)};
    const std::string hole = c.hole;
    centering_.reset();

    const TrayMap* map = nullptr;
    std::string calibration;
    {
      std::lock_guard lock(mutex_);
      map = tray_;
      calibration = status_.fingerprint;
      corrections_.insert_or_assign(hole, correction);
    }
    if (correction_store_ != nullptr && map != nullptr) {
      if (auto saved = correction_store_->put(*map, name_, calibration, hole, correction); !saved) {
        outcome.note = "the correction was not saved: " + saved.error().what;
      }
    }
    std::lock_guard lock(mutex_);
    move_note_ = "hole " + outcome.hole + ": centered, moved " + mm(outcome.moved_mm.x) + ", " + mm(outcome.moved_mm.y) +
                 " mm (residual " + mm(outcome.residual_mm) + " mm)" + (outcome.note.empty() ? "" : "; " + outcome.note);
    outcome_ = std::move(outcome);
    return false;
  }

  // A nudge, unless it would take the stage where only a neighbour can be.
  const StageXY to{here.x + step.move_mm.x, here.y + step.move_mm.y};
  if (apart(to, c.nominal) > c.guard) return give_up(stage, vision::AutocenterReason::MaxTotal);
  if (auto moved = stage.set_xy(to.x, to.y); !moved) {
    abandon(AutocenterOutcome::Result::Failed);
    return fail(std::move(moved).error());
  }
  c.at_rest.reset();
  return true;
}

Result<bool> LaserSystem::give_up(IStage& stage, vision::AutocenterReason why) {
  Centering& c = *centering_;
  c.failed = why;
  c.phase = Centering::Phase::Return;
  c.at_rest.reset();
  if (auto moved = stage.set_xy(c.start.x, c.start.y); !moved) {
    abandon(AutocenterOutcome::Result::Failed);
    return fail(std::move(moved).error());
  }
  return true;
}

Result<void> LaserSystem::set_tray(std::string_view tray) {
  Gate gate(gate_);
  abandon(AutocenterOutcome::Result::Stopped);
  hole_room_ = 0;
  if (tray.empty()) {
    std::lock_guard lock(mutex_);
    tray_ = nullptr;
    status_ = {};
    corrections_.clear();
    last_hole_.clear();
    return {};
  }
  const TrayMap* map = trays_.find(tray);
  if (map == nullptr) return fail(ErrorKind::Config, "no tray map '" + std::string(tray) + "'", name_);
  // Read outside the lock: they are file reads. A missing or stale
  // calibration is not an error until a hole is asked for; corrections that
  // cannot be read are no corrections.
  CalibrationStatus status = calibrations_.status(*map, name_);
  HoleCorrections corrections;
  if (correction_store_ != nullptr && status.state == CalibrationState::Ok) {
    if (auto loaded = correction_store_->load(*map, name_, status.fingerprint)) corrections = std::move(*loaded);
  }
  std::lock_guard lock(mutex_);
  if (tray_ != map) last_hole_.clear();  // the same tray again: the stage is where it was
  tray_ = map;
  status_ = std::move(status);
  corrections_ = std::move(corrections);
  return {};
}

std::vector<std::string> LaserSystem::positions() const {
  std::lock_guard lock(mutex_);
  std::vector<std::string> out;
  if (tray_ != nullptr) {
    for (const auto& hole : tray_->holes()) out.push_back(hole.id);
  }
  return out;
}

}  // namespace pychron::laser
