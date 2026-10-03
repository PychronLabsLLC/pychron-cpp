#pragma once

#include <cstdint>
#include <functional>
#include <variant>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/vision/frame.hpp"
#include "pychron/vision/synth.hpp"

namespace pychron::vision {

struct FrameInfo {
  int width, height;
  std::uint16_t pixel_depth;
  double fps;
};

class IFrameSource {
 public:
  virtual ~IFrameSource() = default;
  virtual Result<Frame> grab() = 0;
  virtual FrameInfo info() const = 0;
};

// Renders a synthetic scene at the stage position reported by StageFn. Frames
// are stamped from ClockFn (never the wall clock) and numbered from 1.
class SyntheticSource final : public IFrameSource {
 public:
  using StageFn = std::function<Vec2()>;
  using ClockFn = std::function<TimePoint()>;

  SyntheticSource(std::variant<HoleScene, GlowScene> scene, StageFn stage, ClockFn clock);

  Result<Frame> grab() override;
  FrameInfo info() const override;
  // Truth of the most recent grab(); default-constructed before the first.
  Truth last_truth() const;

 private:
  std::variant<HoleScene, GlowScene> scene_;
  StageFn stage_;
  ClockFn clock_;
  Truth truth_{};
  std::uint64_t seq_ = 0;
};

}  // namespace pychron::vision
