#pragma once

// The camera of an extraction device (laser autocenter design, section 3):
// <lab>/cameras.toml, one table per device, named as its driver.
//
//   [co2]
//   source = "sim"            sim | recorded | opencv | pylon
//   use = "center"            center | view: a picture only; it never moves the stage
//   px_per_mm = 23.0
//   flip_x = false            image +x is stage -x
//   flip_y = true             image +y is stage -y (the usual camera)
//   aim_offset_px = [0, 0]    the beam's place in the image, from its center
//   settle_ms = 200           after a move, before a frame is trusted
//   frames = "recordings/x"   source = "recorded": a fixture case directory
//
//   [co2.sim]                 source = "sim": the tray as a camera would see it
//   tray_error_mm = [0.15, -0.10]   how far the real tray is from its calibration
//   noise = 0.01
//   width = 200
//   height = 200
//   grain_offset_mm = [0.2, 0.1]      the sample, from its hole's center
//   glow_drift_mm_per_s = [0.01, 0]   how it creeps while heated
//   glow_sigma_mm = 0.3               the size of the glow
//
//   [co2.autocenter]
//   tolerance_mm = 0.03
//   max_iterations = 4
//   max_step_mm = 0.5
//   frames_per_step = 3
//   on_failure = "continue"   continue | fail
//
//   [co2.opencv]              source = "opencv": whatever OpenCV can open
//   device = 0                a camera's index, or a video file
//   width = 1280              asked of the camera; 0 or absent: its own
//   height = 720
//   fps = 30
//   channel = "luma"          luma | r | g | b
//   rotate = 0                0 | 90 | 180 | 270, clockwise
//   roi = [0, 0, 0, 0]        x, y, w, h; no size: the whole frame
//   timeout_ms = 1000         how long anyone waits for a frame: 100 or more,
//                             and 2000 at most for a camera that centers
//                             (an emergency stop may wait that long for it)
//
//   [co2.pylon]               source = "pylon": a Basler camera (GigE, USB3)
//   serial = "40012345"       empty: the first found
//   exposure_us = 10000
//   gain_db = 0
//   pixel_format = "Mono8"
//   packet_size = 1500        GigE
//   timeout_ms = 1000
//
// A live camera (opencv, pylon) is read on a thread of its own
// (vision::LiveFeed). The pylon table is read and checked whether or not
// this build has the driver; opening it says when it has not.
//
// A device with no table has no camera. A table that does not load is a
// problem for that device only.

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/laser/calibration.hpp"
#include "pychron/vision/calibration.hpp"
#include "pychron/vision/camera_backend.hpp"

namespace pychron::laser {

enum class CameraSource { Sim, Recorded, OpenCv, Pylon };
// "sim", "recorded", "opencv", "pylon".
std::string_view to_string(CameraSource source) noexcept;
// Center: it may center holes and follow the glow. View: it is looked
// through, and nothing it sees moves the stage.
enum class CameraUse { Center, View };
enum class OnAutocenterFailure { Continue, Fail };

struct CameraConfig {
  std::string device;
  CameraSource source = CameraSource::Sim;
  double px_per_mm = 23.0;
  bool flip_x = false;
  bool flip_y = true;
  StageXY aim_offset_px{};
  Duration settle{std::chrono::milliseconds(200)};
  std::string frames;  // recorded: the case directory, relative to the lab
  CameraUse use = CameraUse::Center;

  // A live camera: what its backend is asked for.
  std::string live_device;  // opencv: an index or a file ("0" when empty); pylon: a serial number
  int live_width = 0;
  int live_height = 0;
  double live_fps = 0;
  vision::SourceConfig shape;                          // roi, channel, rotation
  std::map<std::string, std::string> backend_options;  // pylon's own keys, as text
  Duration live_timeout{std::chrono::milliseconds(1000)};
  // The pixel scale as measured by jogging the stage (camera_scale.hpp);
  // used instead of px_per_mm and the flips when there is one.
  std::optional<vision::CameraStageMap> measured;

  StageXY sim_tray_error_mm{};
  double sim_noise = 0.01;
  int sim_width = 200;
  int sim_height = 200;
  // While the simulated laser fires the camera sees the sample glow: this
  // far from its hole's center, creeping this fast as it is heated.
  StageXY sim_grain_offset_mm{};
  StageXY sim_glow_drift_mm_per_s{};
  double sim_glow_sigma_mm = 0.3;

  double tolerance_mm = 0.03;
  double max_step_mm = 0.5;
  int max_iterations = 4;
  int frames_per_step = 3;
  OnAutocenterFailure on_failure = OnAutocenterFailure::Continue;

  // Image offset (px) to the stage move (mm) that centers it.
  vision::CameraStageMap map() const;
  // Pixels per millimetre: the measured map's, or px_per_mm.
  double scale_px_per_mm() const;
  bool live() const noexcept { return source == CameraSource::OpenCv || source == CameraSource::Pylon; }
  // What a live camera's backend is asked to open.
  vision::CameraRequest request() const;
  // What picture this camera gives, as text: its source, device, size and
  // the way the picture is cut and turned. A scale measured with one
  // geometry is not another's (px_per_mm and the flips, which a measurement
  // replaces, are not part of it).
  std::string geometry() const;
};

// Whether this camera may be used to move a stage (autocenter). Only a
// camera that follows the stage closes a loop: a recording never does (it
// is for looking at what a finder sees), a simulated camera only over a
// simulated stage (over a real one it would "find" its made-up tray error
// and the laser would be fired there), and a live camera only over a real
// one (a simulated stage is not what it looks at), and never a video file.
// A camera marked use = "view" never does. Config error saying which.
Result<void> usable_for_autocenter(const CameraConfig& config, bool stage_is_simulated);

class CameraLibrary {
 public:
  CameraLibrary() = default;
  // Never fails: a missing file is an empty library.
  static CameraLibrary load(const std::filesystem::path& file);
  static CameraLibrary parse(std::string_view toml, std::string file_name);

  // Null for a device with no table, or whose table did not load.
  const CameraConfig* find(std::string_view device) const;
  std::vector<std::string> devices() const;  // sorted
  // "<file>: <device>.<key>: <what>".
  const std::vector<std::string>& problems() const noexcept { return problems_; }
  // What stops `device` having its camera: its table's problems, or the
  // file's if it could not be read at all.
  std::vector<std::string> problems_of(std::string_view device) const;

 private:
  std::map<std::string, CameraConfig, std::less<>> cameras_;
  std::vector<std::string> problems_;
  std::map<std::string, std::vector<std::string>, std::less<>> by_device_;
  bool unreadable_ = false;  // the whole file: problems_ is everyone's
};

}  // namespace pychron::laser
