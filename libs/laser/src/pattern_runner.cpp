#include "pychron/laser/pattern_runner.hpp"

#include <random>

namespace pychron::laser {

PatternRunner::PatternRunner(std::string device, extraction::IStage& stage, const PatternLibrary& patterns)
    : device_(std::move(device)), stage_(stage), patterns_(patterns) {}

std::vector<std::string> PatternRunner::patterns() const { return patterns_.names(); }

std::string PatternRunner::progress() const {
  std::lock_guard lock(mutex_);
  if (!active_) return {};
  return name_ + ", point " + std::to_string(next_) + " of " + std::to_string(path_.size());
}

void PatternRunner::end() {
  std::lock_guard lock(mutex_);
  active_ = false;
  path_.clear();
  next_ = 0;
  name_.clear();
}

Result<void> PatternRunner::execute_pattern(std::string_view pattern) {
  {
    std::lock_guard lock(mutex_);
    if (active_) {
      return fail(ErrorKind::Config, "pattern " + name_ + " is still running; stop it before starting " +
                                         std::string(pattern), device_);
    }
  }
  const Pattern* found = patterns_.find(pattern);
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
  const std::uint64_t seed = found->seed ? *found->seed : (std::uint64_t{std::random_device{}()} << 32) | std::random_device{}();
  auto offsets = pattern_path(*found, seed);
  if (!offsets) {
    Error e = std::move(offsets).error();
    e.device = device_;
    return fail(std::move(e));
  }
  // The centre is where the stage is now.
  auto centre = stage_.position();
  if (!centre) return fail(std::move(centre).error());
  {
    std::lock_guard lock(mutex_);
    path_.clear();
    for (const auto& o : *offsets) path_.push_back({centre->x + o.x, centre->y + o.y});
    next_ = 0;
    name_ = found->name;
    velocity_ = found->velocity;
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

Result<bool> PatternRunner::running() {
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
