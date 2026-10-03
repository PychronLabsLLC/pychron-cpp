#include "pychron/vision/source.hpp"

#include <utility>

namespace pychron::vision {

SyntheticSource::SyntheticSource(std::variant<HoleScene, GlowScene> scene, StageFn stage, ClockFn clock)
    : scene_(std::move(scene)), stage_(std::move(stage)), clock_(std::move(clock)) {}

Result<Frame> SyntheticSource::grab() {
  const Vec2 stage = stage_ ? stage_() : Vec2{};
  auto [frame, truth] = std::visit([&](const auto& s) { return render(s, stage); }, scene_);
  frame.timestamp = clock_ ? clock_() : TimePoint{};
  frame.seq = ++seq_;
  truth_ = truth;
  return frame;
}

FrameInfo SyntheticSource::info() const {
  return std::visit(
      [](const auto& s) { return FrameInfo{s.width, s.height, s.pixel_depth, 30.0}; }, scene_);
}

Truth SyntheticSource::last_truth() const { return truth_; }

}  // namespace pychron::vision
