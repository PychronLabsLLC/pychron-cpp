#include "pychron/vision/source.hpp"

#include <utility>

namespace pychron::vision {

SyntheticSource::SyntheticSource(std::variant<HoleScene, GlowScene> scene, StageFn stage, ClockFn clock)
    : scene_(std::move(scene)), stage_(std::move(stage)), clock_(std::move(clock)) {}

Result<Frame> SyntheticSource::grab() {
  const Vec2 stage = stage_ ? stage_() : Vec2{};
  const std::uint64_t seq = ++seq_;
  // Mix seq into the seed so successive frames carry independent noise;
  // render() itself stays deterministic for a given scene.
  auto [frame, truth] = std::visit(
      [&](auto s) {
        s.seed += static_cast<std::uint32_t>(seq);
        return render(s, stage);
      },
      scene_);
  frame.timestamp = clock_ ? clock_() : TimePoint{};
  frame.seq = seq;
  truth_ = truth;
  return std::move(frame);
}

FrameInfo SyntheticSource::info() const {
  return std::visit(
      [](const auto& s) { return FrameInfo{s.width, s.height, s.pixel_depth, 30.0}; }, scene_);
}

Truth SyntheticSource::last_truth() const { return truth_; }

}  // namespace pychron::vision
