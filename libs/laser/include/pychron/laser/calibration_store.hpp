#pragma once

// Stage calibrations on disk: <dir>/<device>.<tray>.toml, one per extraction
// device and tray (laser system design, section 3.3).
//
//   schema_version = 1
//   device = "co2"
//   tray = "221-hole"
//   tray_sha256 = "..."        the map the points were taken against
//   center = [12.345, 8.901]   for the reader; recomputed from the points
//   rotation_deg = 0.42
//   scale = 1.0
//   rms_mm = 0.011
//
//   [[points]]
//   hole = "111"
//   x = 12.345
//   y = 8.901
//
// Only the points are authoritative. A calibration made against a different
// version of the map is stale and is not used. The store keeps nothing in
// memory: every call reads the file, so a calibration saved by another
// process is seen at the next call.

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/laser/calibration.hpp"
#include "pychron/laser/tray_map.hpp"

namespace pychron::laser {

struct StoredCalibration {
  std::string device;
  std::string tray;
  std::string tray_sha256;
  std::vector<CalibrationPoint> points;
};

enum class CalibrationState { Missing, Stale, Unsolvable, Ok };

// "not calibrated", "stale", "unsolvable", "ok".
std::string_view to_string(CalibrationState state) noexcept;

struct CalibrationStatus {
  CalibrationState state = CalibrationState::Missing;
  std::optional<Solution> solution;  // set exactly when state is Ok
  std::string why;                   // empty when Ok; otherwise names device and tray
};

class CalibrationStore {
 public:
  explicit CalibrationStore(std::filesystem::path dir);

  const std::filesystem::path& dir() const noexcept { return dir_; }
  std::filesystem::path file(std::string_view device, std::string_view tray) const;

  // nullopt when there is no file. Config error for a name that is not a
  // plain file-name part or a file that does not parse.
  Result<std::optional<StoredCalibration>> load(std::string_view device, std::string_view tray) const;
  // Solves first: points that do not solve are refused and the file is left
  // as it was. Written to a temporary file and renamed into place.
  Result<void> save(const TrayMap& map, std::string_view device, std::span<const CalibrationPoint> points) const;
  Result<void> clear(std::string_view device, std::string_view tray) const;

  // What a move may rely on. Never an error: a file that cannot be used says
  // why.
  CalibrationStatus status(const TrayMap& map, std::string_view device) const;

 private:
  std::filesystem::path dir_;
};

}  // namespace pychron::laser
