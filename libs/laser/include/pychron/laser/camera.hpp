#pragma once

// The camera of an extraction device (laser autocenter design, section 3):
// <lab>/cameras.toml, one table per device, named as its driver.
//
//   [co2]
//   source = "sim"            sim | recorded
//   px_per_mm = 23.0
//   flip_x = false            image +x is stage -x
//   flip_y = true             image +y is stage -y (the usual camera)
//   aim_offset_px = [0, 0]    the beam's place in the image, from its centre
//   settle_ms = 200           after a move, before a frame is trusted
//   frames = "recordings/x"   source = "recorded": a fixture case directory
//
//   [co2.sim]                 source = "sim": the tray as a camera would see it
//   tray_error_mm = [0.15, -0.10]   how far the real tray is from its calibration
//   noise = 0.01
//   width = 200
//   height = 200
//
//   [co2.autocenter]
//   tolerance_mm = 0.03
//   max_iterations = 4
//   max_step_mm = 0.5
//   frames_per_step = 3
//   on_failure = "continue"   continue | fail
//
// A device with no table has no camera. A table that does not load is a
// problem for that device only.

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/laser/calibration.hpp"
#include "pychron/vision/calibration.hpp"

namespace pychron::laser {

enum class CameraSource { Sim, Recorded };
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

  StageXY sim_tray_error_mm{};
  double sim_noise = 0.01;
  int sim_width = 200;
  int sim_height = 200;

  double tolerance_mm = 0.03;
  double max_step_mm = 0.5;
  int max_iterations = 4;
  int frames_per_step = 3;
  OnAutocenterFailure on_failure = OnAutocenterFailure::Continue;

  // Image offset (px) to the stage move (mm) that centres it.
  vision::CameraStageMap map() const;
};

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
