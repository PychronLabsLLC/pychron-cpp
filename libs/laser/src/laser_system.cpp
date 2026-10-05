#include "pychron/laser/laser_system.hpp"

#include "pychron/laser/pattern_runner.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <limits>
#include <optional>

#include "pychron/vision/finder.hpp"

namespace pychron::laser {

using extraction::IStage;
using extraction::StagePosition;

LaserSystem::LaserSystem(std::string name, extraction::IExtractionDevice& driver, const TrayLibrary& trays,
                         const CalibrationStore& calibrations, const PatternLibrary* patterns)
    : name_(std::move(name)), driver_(driver), trays_(trays), calibrations_(calibrations) {
  // The runner moves this system's stage, not the driver's directly, so
  // whatever the system adds to a move applies to a pattern's too.
  if (patterns != nullptr) runner_ = std::make_unique<PatternRunner>(name_, *this, *patterns);
}

LaserSystem::~LaserSystem() = default;

extraction::IPatternRunner* LaserSystem::pattern_runner() {
  if (auto* own = driver_.pattern_runner()) return own;
  return driver_.stage() != nullptr ? runner_.get() : nullptr;
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
  enum class Phase { Centre, Return } phase = Phase::Centre;
  std::string hole;
  std::string tray;
  StageXY nominal{};  // where the calibration puts the hole
  StageXY start{};    // where the centring starts: the correction, or nominal
  double guard = 0;
  vision::Autocenter controller;
  std::optional<TimePoint> at_rest;  // since when the stage has been seen stopped
  int looks = 0;
  int stale = 0;
  double residual = 0;
  vision::AutocenterReason failed = vision::AutocenterReason::None;

  Centering(vision::ITargetFinder& finder, const CameraConfig& camera, vision::AutocenterParams params)
      : controller(finder, camera.map(), camera.px_per_mm, params) {}
};

namespace {

// How many batches of frames no newer than the last decision are waited out
// before a camera is taken to have stopped.
constexpr int kStaleTries = 5;

double apart(StageXY a, StageXY b) { return std::hypot(a.x - b.x, a.y - b.y); }

std::string utc_now() {
  const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm parts{};
#ifdef _WIN32
  gmtime_s(&parts, &now);
#else
  gmtime_r(&now, &parts);
#endif
  char text[32];
  std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%SZ", &parts);
  return text;
}

}  // namespace

void LaserSystem::set_corrections(const CorrectionStore& corrections) { correction_store_ = &corrections; }

void LaserSystem::attach_camera(CameraConfig config, std::unique_ptr<vision::IFrameSource> frames, const Clock& clock) {
  camera_ = std::move(config);
  frames_ = std::move(frames);
  clock_ = &clock;
  finder_ = std::make_unique<vision::SimpleFinder>();
}

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
    TraySight seen;
    if (IStage* stage = driver_.stage()) {
      if (auto at = stage->position()) seen.stage = {at->x, at->y};
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
  auto stage = driver_stage();
  if (!stage) return fail(stage.error());
  abandon(AutocenterOutcome::Result::Stopped);  // a new move takes over

  std::string tray;
  std::optional<StageXY> nominal;
  StageXY start{};
  double guard = 0;
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
    if (autocenter && frames_ != nullptr) {
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
  return moved;
}

Result<void> LaserSystem::set_axis(Axis axis, double value) {
  auto stage = driver_stage();
  if (!stage) return fail(stage.error());
  abandon(AutocenterOutcome::Result::Stopped);
  return (*stage)->set_axis(axis, value);
}

Result<void> LaserSystem::set_xy(double x, double y, double speed_mm_s) {
  auto stage = driver_stage();
  if (!stage) return fail(stage.error());
  abandon(AutocenterOutcome::Result::Stopped);
  return (*stage)->set_xy(x, y, speed_mm_s);
}

Result<void> LaserSystem::stop() {
  auto stage = driver_stage();
  if (!stage) return fail(stage.error());
  abandon(AutocenterOutcome::Result::Stopped);
  return (*stage)->stop();
}

Result<StagePosition> LaserSystem::position() {
  auto stage = driver_stage();
  if (!stage) return fail(stage.error());
  return (*stage)->position();
}

Result<bool> LaserSystem::moving() {
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
    // Back where the centring started: it failed, and now it is over.
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
    centering_.reset();
    {
      std::lock_guard lock(mutex_);
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
  for (int i = 0; i < camera_->frames_per_step; ++i) {
    auto frame = frames_->grab();
    if (!frame) return give_up(stage, vision::AutocenterReason::Camera);
    frames.push_back(std::move(*frame));
  }
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
    // Centred on something: only the hole itself can be this near where the
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
    const HoleCorrection correction{here.x, here.y, c.residual, utc_now()};
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
  abandon(AutocenterOutcome::Result::Stopped);
  if (tray.empty()) {
    std::lock_guard lock(mutex_);
    tray_ = nullptr;
    status_ = {};
    corrections_.clear();
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
