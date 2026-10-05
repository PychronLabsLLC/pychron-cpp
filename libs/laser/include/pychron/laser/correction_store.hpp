#pragma once

// Where autocenter found each hole (laser autocenter design, section 5):
// <dir>/<device>.<tray>.toml, one per extraction device and tray.
//
//   schema_version = 1
//   device = "co2"
//   tray = "221-hole"
//   tray_sha256 = "..."     the map the holes were found on
//   calibration = "..."     fingerprint() of the calibration they were found with
//
//   [holes.17]
//   x = 12.412              stage mm
//   y = 8.877
//   residual_mm = 0.011     the last measured offset
//   found = "2026-10-04T16:20:11Z"
//
// Corrections belong to one version of the map and one calibration: a file
// for another is not used, and is started again by the next put. The store
// keeps nothing in memory; a save goes to a temporary file and is renamed.

#include <filesystem>
#include <map>
#include <string>
#include <string_view>

#include "pychron/core/error.hpp"
#include "pychron/laser/tray_map.hpp"

namespace pychron::laser {

struct HoleCorrection {
  double x = 0;  // stage mm
  double y = 0;
  double residual_mm = 0;
  std::string found;  // UTC, ISO 8601; for the reader
};

using HoleCorrections = std::map<std::string, HoleCorrection, std::less<>>;

class CorrectionStore {
 public:
  explicit CorrectionStore(std::filesystem::path dir);

  const std::filesystem::path& dir() const noexcept { return dir_; }
  std::filesystem::path file(std::string_view device, std::string_view tray) const;

  // The corrections that apply to this map and this calibration: empty when
  // there is no file or it is for another. A hole the map lacks or a value
  // that is not a finite number is left out. Config error for a name that is
  // not a plain file-name part or a file that cannot be understood.
  Result<HoleCorrections> load(const TrayMap& map, std::string_view device, std::string_view calibration) const;
  // Adds or replaces one hole. Config error for a hole the map lacks or a
  // position that is not finite.
  Result<void> put(const TrayMap& map, std::string_view device, std::string_view calibration, std::string_view hole,
                   const HoleCorrection& correction) const;
  Result<void> clear(std::string_view device, std::string_view tray) const;
  Result<void> clear_hole(const TrayMap& map, std::string_view device, std::string_view calibration,
                          std::string_view hole) const;

 private:
  Result<void> write(const TrayMap& map, std::string_view device, std::string_view calibration,
                     const HoleCorrections& holes) const;

  std::filesystem::path dir_;
};

}  // namespace pychron::laser
