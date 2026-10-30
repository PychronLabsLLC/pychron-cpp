#include "pychron/laser/tray_camera.hpp"

#include "pychron/vision/camera_backend.hpp"
#include "pychron/vision/live_feed.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "pychron/core/keyed_noise.hpp"
#include "pychron/vision/fixture.hpp"

namespace pychron::laser {

SimTrayCamera::SimTrayCamera(const CameraConfig& config, TraySightFn sight, const Clock& clock)
    : config_(config), sight_(std::move(sight)), clock_(clock) {}

SimTrayCamera::Nearest SimTrayCamera::nearest_hole(const TraySight& sight, StageXY error) {
  Nearest out;
  out.distance_mm = std::numeric_limits<double>::infinity();
  for (std::size_t i = 0; i < sight.holes.size(); ++i) {
    const auto& hole = sight.holes[i];
    const double d = std::hypot(hole.x + error.x - sight.stage.x, hole.y + error.y - sight.stage.y);
    if (d < out.distance_mm) {
      out.distance_mm = d;
      out.hole = &hole;
      out.index = i;
    }
  }
  return out;
}

void SimTrayCamera::advance_grain(const TraySight& sight, StageXY real, std::size_t index, TimePoint now) {
  if (!grain_ || grain_hole_ != index) {
    // A new hole is a new grain, and it has had no beam on it until now.
    const std::uint64_t seed = keyed_bits(grain_seed_, "grain.hole", static_cast<std::int64_t>(index));
    grain_.emplace(*config_.sim_grain, seed, sight.hole_radius_mm,
                   vision::Vec2{config_.sim_grain_offset_mm.x, config_.sim_grain_offset_mm.y});
    grain_hole_ = index;
    grain_at_ = now;
    return;
  }
  const vision::Vec2 aim = config_.map().to_mm({config_.aim_offset_px.x, config_.aim_offset_px.y});
  const vision::Vec2 beam{sight.stage.x + aim.x - real.x, sight.stage.y + aim.y - real.y};
  grain_->advance(now - grain_at_, beam, sight.firing ? sight.output_percent : 0.0);
  grain_at_ = now;
}

std::optional<GrainView> SimTrayCamera::observe() {
  if (!config_.sim_grain) return std::nullopt;
  const TraySight sight = sight_ ? sight_() : TraySight{};
  const StageXY error = config_.sim_tray_error_mm;
  const Nearest near = nearest_hole(sight, error);
  if (near.hole == nullptr) return std::nullopt;
  const StageXY real{near.hole->x + error.x, near.hole->y + error.y};
  advance_grain(sight, real, near.index, clock_.now());
  const vision::GrainTruth truth = grain_->truth();
  const vision::Vec2 aim = config_.map().to_mm({config_.aim_offset_px.x, config_.aim_offset_px.y});
  return GrainView{{real.x + truth.center_mm.x, real.y + truth.center_mm.y},
                   {sight.stage.x + aim.x, sight.stage.y + aim.y},
                   truth};
}

vision::FrameInfo SimTrayCamera::info() const { return {config_.sim_width, config_.sim_height, 255, 0.0}; }

Result<vision::Frame> SimTrayCamera::grab() {
  const TraySight sight = sight_ ? sight_() : TraySight{};
  const StageXY error = config_.sim_tray_error_mm;

  vision::HoleScene scene;
  scene.width = config_.sim_width;
  scene.height = config_.sim_height;
  scene.px_per_mm = config_.px_per_mm;
  scene.hole_radius_mm = sight.hole_radius_mm;
  scene.noise = config_.sim_noise;
  // Differs frame to frame, repeats run to run. A grain's frames also differ
  // from one grain seed to another.
  scene.seed = config_.sim_grain
                   ? static_cast<std::uint32_t>(keyed_bits(grain_seed_, "grain.frame", static_cast<std::int64_t>(seq_ + 1)))
                   : static_cast<std::uint32_t>(seq_ + 1);

  // The hole nearest the stage, where it really is.
  const Nearest closest = nearest_hole(sight, error);
  const StageXY* nearest = closest.hole;
  const double best = closest.distance_mm;
  // In view: its center within the frame's half diagonal.
  const double reach_mm = 0.5 * std::hypot(scene.width, scene.height) / scene.px_per_mm;
  const bool in_view = nearest != nullptr && best <= reach_mm;
  if (in_view) {
    scene.hole_mm = {nearest->x + error.x, nearest->y + error.y};
  } else {
    // The bare tray: a hole far outside any frame.
    scene.hole_mm = {sight.stage.x + 1e6, sight.stage.y + 1e6};
  }

  // The grain creeps only while it is heated, and stays where it got to.
  const TimePoint now = clock_.now();
  if (sight.firing && lit_since_) {
    const double seconds = std::chrono::duration<double>(now - *lit_since_).count();
    crept_.x += config_.sim_glow_drift_mm_per_s.x * seconds;
    crept_.y += config_.sim_glow_drift_mm_per_s.y * seconds;
  }
  lit_since_ = sight.firing ? std::optional<TimePoint>(now) : std::nullopt;

  // The grain heats under the beam whether or not it is seen, and cools
  // unseen when the beam is off.
  if (config_.sim_grain && nearest != nullptr) {
    advance_grain(sight, {nearest->x + error.x, nearest->y + error.y}, closest.index, now);
  }

  vision::Frame frame;
  vision::Truth truth;
  if (sight.firing && config_.sim_grain) {
    vision::GlowScene glow;
    glow.width = scene.width;
    glow.height = scene.height;
    glow.px_per_mm = scene.px_per_mm;
    glow.noise = scene.noise;
    glow.seed = scene.seed;
    if (grain_ && nearest != nullptr) {
      // The lobes lie from the hole's center; the hole is where it really is.
      for (const auto& lobe : grain_->lobes()) {
        vision::GlowLobe at = lobe;
        at.at_mm = {scene.hole_mm.x + lobe.at_mm.x, scene.hole_mm.y + lobe.at_mm.y};
        if (!in_view) at.peak = 0;  // no hole in view: dark
        glow.lobes.push_back(at);
      }
    }
    if (glow.lobes.empty()) glow.lobes.push_back({scene.hole_mm, config_.sim_grain->lobe_sigma_mm, 0.0});
    const bool lit = std::any_of(glow.lobes.begin(), glow.lobes.end(), [](const auto& l) { return l.peak > 0; });
    auto drawn = vision::render(glow, {sight.stage.x, sight.stage.y});
    frame = std::move(drawn.first);
    truth = drawn.second;
    truth.visible = truth.visible && in_view && lit;
  } else if (sight.firing) {
    // Under the beam the picture is the glow, not the holes.
    vision::GlowScene glow;
    glow.width = scene.width;
    glow.height = scene.height;
    glow.px_per_mm = scene.px_per_mm;
    glow.noise = scene.noise;
    glow.seed = scene.seed;
    glow.sigma_mm = config_.sim_glow_sigma_mm;
    // Brighter with output: followable at a working output, saturated flat out.
    glow.peak = in_view ? std::clamp(0.03 * sight.output_percent, 0.0, 1.3) : 0.0;
    glow.glow_mm = {scene.hole_mm.x + config_.sim_grain_offset_mm.x + crept_.x,
                    scene.hole_mm.y + config_.sim_grain_offset_mm.y + crept_.y};
    auto lit = vision::render(glow, {sight.stage.x, sight.stage.y});
    frame = std::move(lit.first);
    truth = lit.second;
    truth.visible = truth.visible && in_view && glow.peak > 0;
  } else {
    auto drawn = vision::render(scene, {sight.stage.x, sight.stage.y});
    frame = std::move(drawn.first);
    truth = drawn.second;
    truth.visible = truth.visible && in_view;
  }

  // The tray's other holes that are in view, where they really are (not on
  // an imagined grid: a finder must not be offered holes the tray lacks).
  // Holes are dark on the tray, so the darker pixel wins. Each is drawn
  // where it is, not as a frame of its own: a live picture is many frames a
  // second, on a tray of many holes.
  vision::HoleScene other = scene;
  for (const auto& hole : sight.holes) {
    if (&hole == nearest || !in_view || sight.firing) continue;
    const StageXY real{hole.x + error.x, hole.y + error.y};
    if (std::hypot(real.x - sight.stage.x, real.y - sight.stage.y) > reach_mm + sight.hole_radius_mm) continue;
    other.hole_mm = {real.x, real.y};
    vision::darken_hole(frame, other, {sight.stage.x, sight.stage.y});
  }

  // The vision library renders the usual camera: image +x is stage +x, image
  // +y is stage -y. A camera the config says is mounted otherwise gives the
  // mirrored picture.
  const bool mirror_x = config_.flip_x;
  const bool mirror_y = !config_.flip_y;
  if (mirror_x || mirror_y) {
    vision::Frame out = vision::Frame::make(frame.width, frame.height, frame.pixel_depth);
    for (int y = 0; y < frame.height; ++y) {
      for (int x = 0; x < frame.width; ++x) {
        out.at(mirror_x ? frame.width - 1 - x : x, mirror_y ? frame.height - 1 - y : y) = frame.at(x, y);
      }
    }
    if (mirror_x) truth.center_px.x = (frame.width - 1) - truth.center_px.x;
    if (mirror_y) truth.center_px.y = (frame.height - 1) - truth.center_px.y;
    frame = std::move(out);
  }

  frame.timestamp = now;
  frame.seq = ++seq_;
  truth_ = truth;
  return frame;
}

Result<std::unique_ptr<vision::IFrameSource>> make_frame_source(const CameraConfig& config,
                                                                const std::filesystem::path& lab, TraySightFn sight,
                                                                const Clock& clock) {
  if (config.source == CameraSource::Recorded) {
    const auto dir = lab / config.frames;
    auto recorded = vision::load_case(dir);
    if (!recorded) {
      return fail(ErrorKind::Config,
                  "camera of " + config.device + ": " + dir.string() + " is not a recording (" + recorded.error().what + ")",
                  config.device);
    }
    const Clock* stamp = &clock;
    return std::unique_ptr<vision::IFrameSource>(
        std::make_unique<vision::RecordedSource>(std::move(*recorded), [stamp] { return stamp->now(); }));
  }
  if (config.live()) {
    const vision::CameraRequest request = config.request();
    // Known before anything is opened: a backend this build does not have
    // will not appear by waiting.
    bool known = false;
    for (const auto& backend : vision::camera_backends()) {
      if (backend.name != request.backend) continue;
      known = true;
      if (!backend.available) {
        return fail(ErrorKind::Config, "camera of " + config.device + ": " + backend.unavailable_why, config.device);
      }
    }
    if (!known) {
      return fail(ErrorKind::Config, "camera of " + config.device + ": no camera backend " + request.backend,
                  config.device);
    }
    vision::LiveFeedOptions options;
    options.timeout = std::chrono::duration_cast<std::chrono::milliseconds>(config.live_timeout);
    const Clock* stamp = &clock;
    options.stamp = [stamp] { return stamp->now(); };
    // A camera that is not there yet is waited for: the feed goes on trying.
    auto feed = std::make_unique<vision::LiveFeed>(
        [request](vision::ClockFn frame_clock) { return vision::open_camera(request, std::move(frame_clock)); }, options);
    (void)feed->wait_open();
    return std::unique_ptr<vision::IFrameSource>(std::move(feed));
  }
  return std::unique_ptr<vision::IFrameSource>(std::make_unique<SimTrayCamera>(config, std::move(sight), clock));
}

}  // namespace pychron::laser
