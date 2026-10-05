#pragma once

// The frames a device's camera gives (laser autocenter design, section 4).
//
//   sim        SimTrayCamera: the current tray as a camera looking down the
//              beam path would see it, from wherever the stage is. The real
//              tray sits `sim.tray_error_mm` from where its calibration says:
//              that is what autocenter has to find.
//   recorded   vision::RecordedSource over a fixture case directory. It does
//              not follow the stage: for looking at what a finder sees, not
//              for closing the loop.
//
// A live camera (screen capture of Chromium's video window) is a later
// source of the same interface.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/laser/calibration.hpp"
#include "pychron/laser/camera.hpp"
#include "pychron/vision/source.hpp"
#include "pychron/vision/synth.hpp"

namespace pychron::laser {

// What the camera is over: where the stage is, and where the calibration
// says the current tray's holes are (stage mm). No tray: no holes.
struct TraySight {
  StageXY stage{};
  std::vector<StageXY> holes;
  double hole_radius_mm = 0.5;
  bool firing = false;        // the beam is on
  double output_percent = 0;  // at this output
};
using TraySightFn = std::function<TraySight()>;

class SimTrayCamera final : public vision::IFrameSource {
 public:
  // `sight` is asked at every grab; `clock` stamps the frames. Both must
  // outlive the camera.
  SimTrayCamera(const CameraConfig& config, TraySightFn sight, const Clock& clock);

  // The tray's holes that are in view, each at its real position; the bare
  // tray when none is. While the laser fires: the sample's glow instead, at
  // the nearest hole's real position plus the grain's offset and however far
  // it has crept, as bright as the output makes it. Numbered from 1.
  Result<vision::Frame> grab() override;
  vision::FrameInfo info() const override;
  // Of the hole in the most recent frame; not visible when there was none.
  vision::Truth last_truth() const { return truth_; }

 private:
  CameraConfig config_;
  TraySightFn sight_;
  const Clock& clock_;
  vision::Truth truth_{};
  std::uint64_t seq_ = 0;
  StageXY crept_{};                      // how far the grain has drifted under the beam
  std::optional<TimePoint> lit_since_;   // the last grab with the beam on
};

// A live source (opencv, pylon) comes behind a vision::LiveFeed, which goes
// on trying to open a camera that is not there yet (what is wrong is in its
// latest()). Config error for a recorded source whose directory is not a
// fixture case, and for a live source whose backend this build does not have.
Result<std::unique_ptr<vision::IFrameSource>> make_frame_source(const CameraConfig& config,
                                                                const std::filesystem::path& lab, TraySightFn sight,
                                                                const Clock& clock);

}  // namespace pychron::laser
