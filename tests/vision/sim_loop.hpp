#pragma once

// Closed-loop helpers: a simulated stage and a way to render the frames a
// camera would deliver at its position. Generic over HoleScene and GlowScene.

#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

#include "pychron/vision/frame.hpp"
#include "pychron/vision/synth.hpp"
#include "pychron/vision/types.hpp"

namespace pychron::vision::sim {

struct SimStage {
  Vec2 pos;
  void move(Vec2 d) {
    pos.x += d.x;
    pos.y += d.y;
  }
};

// Owns the frames; build a std::span<const FrameView> from views().
struct FrameSet {
  std::vector<Frame> frames;
  std::vector<FrameView> views() const {
    std::vector<FrameView> v;
    v.reserve(frames.size());
    for (const Frame& f : frames) v.push_back(f.view());
    return v;
  }
};

// Timestamps and sequence numbers increase across calls through `next_seq`;
// the seed changes per frame so each frame gets its own noise.
template <class Scene>
FrameSet render_frames(const Scene& scene, const SimStage& stage, int n, std::uint64_t& next_seq) {
  FrameSet set;
  for (int i = 0; i < n; ++i) {
    Scene s = scene;
    s.seed = scene.seed + static_cast<std::uint32_t>(next_seq) * 7919u;
    Frame f = render(s, stage.pos).first;
    f.seq = next_seq;
    f.timestamp = TimePoint{} + std::chrono::milliseconds(33 * static_cast<std::int64_t>(next_seq + 1));
    ++next_seq;
    set.frames.push_back(std::move(f));
  }
  return set;
}

// A featureless frame of the same size, for "target lost" frames.
template <class Scene>
Frame blank_frame(const Scene& scene, std::uint64_t seq = 0) {
  Frame f = Frame::make(scene.width, scene.height, scene.pixel_depth,
                        static_cast<std::uint16_t>(scene.pixel_depth / 2));
  f.seq = seq;
  f.timestamp = TimePoint{} + std::chrono::milliseconds(33 * static_cast<std::int64_t>(seq + 1));
  return f;
}

}  // namespace pychron::vision::sim
