#pragma once

// A camera's pixel scale, measured (live camera design, section 4). Typing
// px_per_mm and the flips into cameras.toml is a guess until it is checked,
// and a wrong sign sends a centring away from its hole. With something the
// finder can see under the aim point, three sightings settle it: where the
// target is, where it is after the stage moves a step in x, and after a step
// in y.
//
// A measurement is kept in <lab>/camera_scales/<device>.toml and used
// instead of what cameras.toml says, which is not rewritten.

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string_view>

#include "pychron/core/error.hpp"
#include "pychron/vision/calibration.hpp"

namespace pychron::laser {

class LaserSystem;

struct ScaleMeasurement {
  vision::CameraStageMap map;  // image offset (px) to the stage move (mm) that centres it
  double px_per_mm = 0;
  bool flip_x = false;      // as cameras.toml's: image +x is stage -x
  bool flip_y = false;
  double skew_deg = 0;      // how far the two axes are from square
  double anisotropy = 0;    // how far they disagree about the scale, as a fraction
};

// The two axes may disagree about the scale by this much, and be this far
// from square, before a measurement is refused: more is a sighting of the
// wrong thing, not a camera.
inline constexpr double kMaxAnisotropy = 0.03;
inline constexpr double kMaxSkewDeg = 3.0;

// Config error for fewer than two jogs, jogs along one line, a target that
// did not move, or axes outside the rules above.
Result<ScaleMeasurement> scale_from(std::span<const vision::JogPair> jogs);

// Looks, moves the stage `step_mm` in x, looks, moves it `step_mm` in y from
// where it started, looks, and puts it back. `arrive` waits for a move to end
// and the picture to settle (it polls the system's moving()). The target
// followed is the one nearest the aim point at the start. Config error with
// no camera, for a step outside 0.01 to 2 mm, and when nothing is seen at
// one of the three places; the stage is put back whatever happens, unless it
// is the stage that failed. Nothing is saved or applied here.
Result<ScaleMeasurement> measure_camera_scale(LaserSystem& system, double step_mm,
                                              const std::function<Result<void>()>& arrive);

class CameraScaleStore {
 public:
  explicit CameraScaleStore(std::filesystem::path dir);
  std::filesystem::path file(std::string_view device) const;
  // nullopt when there is no file. Config error for a device that is not a
  // plain file-name part, and for a file that does not hold a measurement.
  Result<std::optional<ScaleMeasurement>> load(std::string_view device) const;
  Result<void> save(std::string_view device, const ScaleMeasurement& measurement) const;
  Result<void> clear(std::string_view device) const;

 private:
  std::filesystem::path dir_;
};

}  // namespace pychron::laser
