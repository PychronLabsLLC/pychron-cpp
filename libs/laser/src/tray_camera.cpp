#include "pychron/laser/tray_camera.hpp"

#include "pychron/vision/camera_backend.hpp"
#include "pychron/vision/live_feed.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "pychron/vision/fixture.hpp"

namespace pychron::laser {

SimTrayCamera::SimTrayCamera(const CameraConfig& config, TraySightFn sight, const Clock& clock)
    : config_(config), sight_(std::move(sight)), clock_(clock) {}

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
  scene.seed = static_cast<std::uint32_t>(seq_ + 1);  // differs frame to frame, repeats run to run

  // The hole nearest the stage, where it really is.
  const StageXY* nearest = nullptr;
  double best = std::numeric_limits<double>::infinity();
  for (const auto& hole : sight.holes) {
    const double d = std::hypot(hole.x + error.x - sight.stage.x, hole.y + error.y - sight.stage.y);
    if (d < best) {
      best = d;
      nearest = &hole;
    }
  }
  // In view: its centre within the frame's half diagonal.
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

  vision::Frame frame;
  vision::Truth truth;
  if (sight.firing) {
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
  // Holes are dark on the tray, so the darker pixel wins.
  vision::HoleScene other = scene;
  other.noise = 0;
  for (const auto& hole : sight.holes) {
    if (&hole == nearest || !in_view || sight.firing) continue;
    const StageXY real{hole.x + error.x, hole.y + error.y};
    if (std::hypot(real.x - sight.stage.x, real.y - sight.stage.y) > reach_mm + sight.hole_radius_mm) continue;
    other.hole_mm = {real.x, real.y};
    const vision::Frame drawn = vision::render(other, {sight.stage.x, sight.stage.y}).first;
    for (std::size_t i = 0; i < frame.data.size() && i < drawn.data.size(); ++i) {
      frame.data[i] = std::min(frame.data[i], drawn.data[i]);
    }
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
  return std::move(frame);
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
