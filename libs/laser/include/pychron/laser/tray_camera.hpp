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
#include "pychron/vision/grain.hpp"
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

// Where the simulated grain really is, and the beam, in stage mm.
struct GrainView {
  StageXY grain_mm;
  StageXY beam_mm;
  vision::GrainTruth truth;  // relative to the hole's center
};

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

  // With a [sim.grain] table (CameraConfig::sim_grain) the glow is a
  // vision::GrainModel that heats where the beam is, flares and moves
  // (the dragonfly grain simulator design, section 5). The beam is the stage
  // plus the aim offset. A new nearest hole is a new grain, its seed mixed
  // with the hole's index.
  //
  // The seed of the grain, before the first grab; 1 until then.
  void set_grain_seed(std::uint64_t seed) { grain_seed_ = seed; }
  // Runs the model up to the clock's now without drawing a frame, and says
  // where the grain and the beam are. Empty without a [sim.grain] table or
  // with no hole in the tray. A grab after it gives the frame it would have
  // given without it.
  std::optional<GrainView> observe();

 private:
  struct Nearest {
    const StageXY* hole = nullptr;  // in the sight's list
    std::size_t index = 0;
    double distance_mm = 0;
  };
  static Nearest nearest_hole(const TraySight& sight, StageXY error);
  // Brings the model to `now` for the hole nearest the stage: made new when
  // that is another hole than the model's. The hole's real position (stage
  // mm) is `real`. One place for grab() and observe(), so both move the
  // model by the same dt.
  void advance_grain(const TraySight& sight, StageXY real, std::size_t index, TimePoint now);
  CameraConfig config_;
  TraySightFn sight_;
  const Clock& clock_;
  vision::Truth truth_{};
  std::uint64_t seq_ = 0;
  StageXY crept_{};                      // how far the grain has drifted under the beam
  std::optional<TimePoint> lit_since_;   // the last grab with the beam on
  std::optional<vision::GrainModel> grain_;
  std::size_t grain_hole_ = 0;           // index of the hole grain_ lies in
  TimePoint grain_at_{};                 // how far grain_ has been run
  std::uint64_t grain_seed_ = 1;
};

// A live source (opencv, pylon) comes behind a vision::LiveFeed, which goes
// on trying to open a camera that is not there yet (what is wrong is in its
// latest()). Config error for a recorded source whose directory is not a
// fixture case, and for a live source whose backend this build does not have.
Result<std::unique_ptr<vision::IFrameSource>> make_frame_source(const CameraConfig& config,
                                                                const std::filesystem::path& lab, TraySightFn sight,
                                                                const Clock& clock);

}  // namespace pychron::laser
