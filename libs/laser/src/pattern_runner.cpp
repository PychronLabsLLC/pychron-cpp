#include "pychron/laser/pattern_runner.hpp"

#include <chrono>
#include <cmath>
#include <iomanip>
#include <locale>
#include <optional>
#include <random>
#include <sstream>
#include <utility>

#include "pychron/vision/dragonfly.hpp"

namespace pychron::laser {

// A dragonfly in progress.
struct PatternRunner::Follow {
  enum class Phase { Follow, Return, Hold } phase = Phase::Follow;
  vision::Dragonfly controller;
  StageXY center{};
  double perimeter = 0;
  double velocity = 0;
  double duration_s = 0;
  TimePoint ends{};
  std::optional<TimePoint> at_rest;
  int stale = 0;
  std::string lost;  // why the camera was given up on; empty: it was not
  double own_perimeter = 0;  // the pattern's, before the hole's room limited it
  bool looked = false;       // a frame was taken
  bool seen = false;         // the glow was found in one

  Follow(vision::ITargetFinder& finder, const CameraConfig& camera, const vision::DragonflyParams& params)
      : controller(finder, camera.map(), camera.scale_px_per_mm(), params) {}
};

namespace {

// How many batches of frames the controller will not take (older than its
// last move) are waited out before the camera is taken to have stopped.
constexpr int kStaleTries = 5;

std::string mm_text(double mm) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::fixed << std::setprecision(3) << mm;
  return out.str();
}

std::string seconds_text(double seconds) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << seconds;
  return out.str();
}

}  // namespace

PatternRunner::PatternRunner(std::string device, extraction::IStage& stage, const PatternLibrary& patterns)
    : device_(std::move(device)), stage_(stage), patterns_(patterns) {}

PatternRunner::~PatternRunner() = default;

void PatternRunner::set_vision(PatternVision vision) {
  vision_ = vision;
  if (vision_ && finder_ == nullptr) finder_ = std::make_unique<vision::SimpleFinder>();
}

std::string PatternRunner::last_note() {
  std::lock_guard lock(mutex_);
  return std::exchange(note_, {});
}

std::vector<std::string> PatternRunner::patterns() const { return patterns_.names(); }

std::string PatternRunner::progress() const {
  std::lock_guard lock(mutex_);
  if (!active_) return {};
  if (path_.empty()) return name_ + ", following the glow";
  return name_ + ", point " + std::to_string(next_) + " of " + std::to_string(path_.size());
}

void PatternRunner::end() {
  follow_.reset();
  std::lock_guard lock(mutex_);
  active_ = false;
  path_.clear();
  next_ = 0;
  name_.clear();
}

Result<void> PatternRunner::execute_pattern_for(std::string_view pattern, double duration_s) {
  {
    std::lock_guard lock(mutex_);
    if (active_) {
      return fail(ErrorKind::Config, "pattern " + name_ + " is still running; stop it before starting " +
                                         std::string(pattern), device_);
    }
  }
  const std::shared_ptr<const Pattern> found = patterns_.find(pattern);
  if (found == nullptr) {
    // A file that is there and did not load says why.
    for (const auto& problem : patterns_.problems()) {
      if (problem.starts_with(std::string(pattern) + ": ")) return fail(ErrorKind::Config, "pattern " + problem, device_);
    }
    std::string known;
    for (const auto& name : patterns_.names()) known += (known.empty() ? "" : ", ") + name;
    return fail(ErrorKind::Config,
                "unknown pattern " + std::string(pattern) + " (there is: " + (known.empty() ? "none" : known) + ")",
                device_);
  }
  if (found->follows_glow()) return start_following(*found, duration_s);
  // Only a random walk with no seed of its own needs one made up.
  std::uint64_t seed = found->seed.value_or(0);
  if (found->kind == PatternKind::Random && !found->seed) {
    std::random_device entropy;
    seed = (std::uint64_t{entropy()} << 32) | entropy();
  }
  auto offsets = pattern_path(*found, seed);
  if (!offsets) {
    Error e = std::move(offsets).error();
    e.device = device_;
    return fail(std::move(e));
  }
  // The center is where the stage is now: it has to have stopped.
  auto moving = stage_.moving();
  if (!moving) return fail(std::move(moving).error());
  if (*moving) {
    return fail(ErrorKind::Config, "the stage is still moving; pattern " + found->name + " starts from where it stops",
                device_);
  }
  auto center = stage_.position();
  if (!center) return fail(std::move(center).error());
  {
    std::lock_guard lock(mutex_);
    path_.clear();
    for (const auto& o : *offsets) path_.push_back({center->x + o.x, center->y + o.y});
    next_ = 0;
    name_ = found->name;
    velocity_ = found->velocity;
    note_.clear();
    active_ = true;
  }
  return send_next();
}

Result<void> PatternRunner::send_next() {
  StageXY to;
  double velocity = 0;
  std::string where;
  {
    std::lock_guard lock(mutex_);
    if (!active_ || next_ >= path_.size()) return {};  // stopped meanwhile
    to = path_[next_];
    velocity = velocity_;
    ++next_;
    where = "pattern " + name_ + ", point " + std::to_string(next_) + " of " + std::to_string(path_.size()) + ": ";
  }
  auto moved = stage_.set_xy(to.x, to.y, velocity);
  if (!moved) {
    end();
    Error e = std::move(moved).error();
    e.what = where + e.what;
    return fail(std::move(e));
  }
  return {};
}

Result<void> PatternRunner::start_following(const Pattern& pattern, double run_duration_s) {
  if (!vision_) {
    return fail(ErrorKind::Config,
                "pattern " + pattern.name + " follows the glow and " + device_ + " has no camera to see it", device_);
  }
  auto moving = stage_.moving();
  if (!moving) return fail(std::move(moving).error());
  if (*moving) {
    return fail(ErrorKind::Config, "the stage is still moving; pattern " + pattern.name + " starts from where it stops",
                device_);
  }
  auto center = stage_.position();
  if (!center) return fail(std::move(center).error());

  const CameraConfig& camera = *vision_.camera;
  // The run's duration, as in legacy pychron; the pattern's own when the run has none.
  const double duration_s = run_duration_s > 0 ? run_duration_s : pattern.duration_s;
  if (!(duration_s > 0) || !std::isfinite(duration_s)) {
    return fail(ErrorKind::Config,
                "pattern " + pattern.name + " has no duration: the run gives none and the pattern has none of its own",
                device_);
  }
  // What it looks at must be a picture: the crop is 2.5 target diameters.
  const double side_px = 2.5 * 2.0 * pattern.target_radius * camera.scale_px_per_mm();
  if (!(side_px >= 1.0 && side_px <= 1.0e5)) {
    return fail(ErrorKind::Config,
                "pattern " + pattern.name + ": target_radius gives a look of " + seconds_text(side_px) +
                    " px with this camera, which cannot show a glow",
                device_);
  }
  // Never beyond the room its hole has: the beam is on.
  double perimeter = pattern.perimeter_radius;
  const double room = vision_.room_mm ? vision_.room_mm() : 0.0;
  if (room > 0 && room < perimeter) perimeter = room;

  vision::DragonflyParams params;
  params.total_duration = std::chrono::duration_cast<Duration>(std::chrono::duration<double>(duration_s));
  params.perimeter_radius_mm = perimeter;
  params.saturation_threshold = pattern.saturation_threshold;
  params.aggressiveness = pattern.aggressiveness;
  params.move_threshold_mm = pattern.move_threshold;
  params.max_step_mm = pattern.max_step;
  params.spiral = pattern.square_spiral ? vision::SpiralKind::Square : vision::SpiralKind::Hexagon;
  params.spiral_base_mm = pattern.spiral_base;
  params.frames_per_step = camera.frames_per_step;
  params.aim_offset_px = {camera.aim_offset_px.x, camera.aim_offset_px.y};
  params.target_radius_mm = pattern.target_radius;

  const TimePoint now = vision_.clock->now();
  follow_ = std::make_unique<Follow>(*finder_, camera, params);
  follow_->center = {center->x, center->y};
  follow_->perimeter = perimeter;
  follow_->own_perimeter = pattern.perimeter_radius;
  follow_->velocity = pattern.velocity;
  follow_->duration_s = duration_s;
  follow_->ends = now + params.total_duration;
  follow_->controller.start(now, {center->x, center->y});
  std::lock_guard lock(mutex_);
  path_.clear();
  next_ = 0;
  name_ = pattern.name;
  velocity_ = pattern.velocity;
  note_.clear();
  active_ = true;
  return {};
}

Result<bool> PatternRunner::go_home() {
  Follow& f = *follow_;
  f.phase = Follow::Phase::Return;
  f.at_rest.reset();
  if (auto moved = stage_.set_xy(f.center.x, f.center.y, f.velocity); !moved) {
    const std::string name = name_;
    end();
    Error e = std::move(moved).error();
    e.what = "pattern " + name + ": " + e.what;
    return fail(std::move(e));
  }
  return true;
}

Result<bool> PatternRunner::lose_the_camera(std::string why) {
  follow_->lost = std::move(why);
  {
    // Said now: the pattern may be stopped before it has run its time.
    std::lock_guard lock(mutex_);
    note_ = "pattern " + name_ + ": the camera was lost (" + follow_->lost + ")";
  }
  return go_home();
}

Result<bool> PatternRunner::follow() {
  Follow& f = *follow_;
  const std::string name = name_;  // the caller's thread wrote it
  const auto failed = [&](Error e) {
    end();
    e.what = "pattern " + name + ": " + e.what;
    return fail(std::move(e));
  };

  auto moving = stage_.moving();
  if (!moving) return failed(std::move(moving).error());
  if (*moving) {
    f.at_rest.reset();
    return true;
  }
  const TimePoint now = vision_.clock->now();

  if (f.phase == Follow::Phase::Return) {
    if (f.lost.empty()) {  // its time was up
      // What it could not do, for the run's log.
      std::string said;
      const auto add = [&said](const std::string& what) { said += (said.empty() ? "" : "; ") + what; };
      if (!f.looked) {
        add("it never looked: its " + seconds_text(f.duration_s) + " s were over before the stage had settled");
      } else if (!f.seen) {
        add("the glow was not seen: it searched for its " + seconds_text(f.duration_s) + " s");
      }
      // Only when it went looking: a pattern that had its glow never needed more room.
      if (f.looked && !f.seen && f.perimeter < f.own_perimeter) {
        add("it was held within " + mm_text(f.perimeter) + " mm of its start, the room the hole has (its perimeter is " +
            mm_text(f.own_perimeter) + " mm)");
      }
      end();
      if (!said.empty()) {
        std::lock_guard lock(mutex_);
        note_ = "pattern " + name + ": " + said;
      }
      return false;
    }
    if (vision_.camera->on_failure == OnAutocenterFailure::Fail) {
      const std::string why = f.lost;
      end();
      return fail(ErrorKind::Config, "pattern " + name + ": the camera was lost (" + why + "); the stage is back where it started",
                  device_);
    }
    f.phase = Follow::Phase::Hold;  // a plain timed heat for what is left
  }
  if (f.phase == Follow::Phase::Hold) {
    if (now < f.ends) return true;
    const std::string said = "pattern " + name + ": the camera was lost (" + f.lost +
                             "); the stage was held where the pattern started for the rest of its " +
                             seconds_text(f.duration_s) + " s";
    end();
    std::lock_guard lock(mutex_);
    note_ = said;
    return false;
  }

  // Following. Time first: whatever else, it ends when its time is up.
  if (now >= f.ends) return go_home();
  // Frames are trusted only once the stage has been at rest for the settle time.
  if (!f.at_rest) f.at_rest = now;
  if (now - *f.at_rest < vision_.camera->settle) return true;

  std::vector<vision::Frame> frames;
  // One wait for the whole look, not one per frame: the device is held meanwhile.
  const auto began = std::chrono::steady_clock::now();
  for (int i = 0; i < vision_.camera->frames_per_step; ++i) {
    auto frame = vision_.frames->grab();
    if (!frame) return lose_the_camera(frame.error().what);
    frames.push_back(std::move(*frame));
    if (vision_.camera->live() && std::chrono::steady_clock::now() - began > vision_.camera->live_timeout &&
        i + 1 < vision_.camera->frames_per_step) {
      return lose_the_camera("the camera is too slow: its frames did not come within its timeout");
    }
  }
  f.looked = true;
  std::vector<vision::FrameView> views;
  for (const auto& frame : frames) views.push_back(frame.view());
  auto at = stage_.position();
  if (!at) return failed(std::move(at).error());

  auto step = f.controller.step(views, now, {at->x, at->y});
  if (!step) {
    // Frames older than its last move: the camera may only be slow.
    if (++f.stale < kStaleTries) return true;
    return lose_the_camera(step.error().what);
  }
  f.stale = 0;
  using Action = vision::DragonflyStep::Action;
  if (step->action == Action::Done) return go_home();
  if (step->reason == vision::DragonflyStep::Reason::Invalid) {
    return failed(Error{ErrorKind::Config, "its settings or the camera's cannot be used to follow a glow", device_});
  }
  using Reason = vision::DragonflyStep::Reason;
  if (step->reason == Reason::Saturated || step->reason == Reason::Deadband || step->reason == Reason::Track) {
    f.seen = true;  // it has a glow to go by
  }
  if (step->action == Action::Hold) return true;

  // An absolute move: the target is an offset from where the pattern started,
  // which the controller keeps within the perimeter it was given.
  const StageXY to{f.center.x + step->target_mm.x, f.center.y + step->target_mm.y};
  if (auto moved = stage_.set_xy(to.x, to.y, f.velocity); !moved) return failed(std::move(moved).error());
  f.at_rest.reset();
  return true;
}

Result<bool> PatternRunner::running() {
  if (follow_ != nullptr) return follow();
  std::string where;
  bool last = false;
  {
    std::lock_guard lock(mutex_);
    if (!active_) return false;
    where = "pattern " + name_ + ", point " + std::to_string(next_) + " of " + std::to_string(path_.size()) + ": ";
    last = next_ >= path_.size();
  }
  auto moving = stage_.moving();
  if (!moving) {
    end();
    Error e = std::move(moving).error();
    e.what = where + e.what;
    return fail(std::move(e));
  }
  if (*moving) return true;
  if (last) {
    end();
    return false;
  }
  if (auto sent = send_next(); !sent) return fail(std::move(sent).error());
  return true;
}

Result<void> PatternRunner::stop_pattern() {
  {
    std::lock_guard lock(mutex_);
    if (!active_) return {};
  }
  end();
  auto stopped = stage_.stop();
  // A stage that cannot stop finishes its move; the pattern is over either way.
  if (!stopped && !extraction::is_not_supported(stopped.error())) return stopped;
  return {};
}

}  // namespace pychron::laser
