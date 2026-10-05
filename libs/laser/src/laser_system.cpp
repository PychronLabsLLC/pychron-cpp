#include "pychron/laser/laser_system.hpp"

#include "pychron/laser/pattern_runner.hpp"

#include <optional>

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

Result<void> LaserSystem::move_to_position(std::string_view position, bool /*autocenter*/) {
  auto stage = driver_stage();
  if (!stage) return fail(stage.error());

  std::string tray;
  std::optional<StageXY> target;
  CalibrationStatus status;
  bool is_hole = false;
  {
    std::lock_guard lock(mutex_);
    if (tray_ != nullptr) {
      tray = tray_->name();
      if (const Hole* hole = tray_->find(position)) {
        is_hole = true;
        status = status_;
        if (status.solution) target = status.solution->transform.to_stage(hole->x, hole->y);
      }
    }
  }

  if (is_hole) {
    if (!target) return fail(ErrorKind::Config, status.why, name_);
    auto moved = (*stage)->set_xy(target->x, target->y);
    if (!moved) {
      Error e = std::move(moved).error();
      e.what = "hole " + std::string(position) + " on " + tray + ": " + e.what;
      return fail(std::move(e));
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
  return (*stage)->set_axis(axis, value);
}

Result<void> LaserSystem::set_xy(double x, double y, double speed_mm_s) {
  auto stage = driver_stage();
  if (!stage) return fail(stage.error());
  return (*stage)->set_xy(x, y, speed_mm_s);
}

Result<void> LaserSystem::stop() {
  auto stage = driver_stage();
  if (!stage) return fail(stage.error());
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
  return (*stage)->moving();
}

Result<void> LaserSystem::set_tray(std::string_view tray) {
  if (tray.empty()) {
    std::lock_guard lock(mutex_);
    tray_ = nullptr;
    status_ = {};
    return {};
  }
  const TrayMap* map = trays_.find(tray);
  if (map == nullptr) return fail(ErrorKind::Config, "no tray map '" + std::string(tray) + "'", name_);
  // Read outside the lock: it is a file read. A missing or stale
  // calibration is not an error until a hole is asked for.
  CalibrationStatus status = calibrations_.status(*map, name_);
  std::lock_guard lock(mutex_);
  tray_ = map;
  status_ = std::move(status);
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
