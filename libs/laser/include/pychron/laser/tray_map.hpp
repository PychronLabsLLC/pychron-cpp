#pragma once

// A sample tray: named holes at positions in millimetres, in the tray's own
// frame. Read from legacy pychron's tray map files as they are
// (docs/superpowers/specs/2026-10-04-laser-system-design.md, section 3.1):
//
//   # comment
//   circle,1.0            shape (circle|square), hole dimension in mm
//   1,2,3                 valid hole ids (may be empty; read and not enforced)
//   3,119,219,103,111     calibration holes: north, east, south, west, centre
//   -3.9878, 15.9512      x,y | id,x,y | x,y,(assoc) | x,y,r<dim> | id,x,y,(assoc)
//
// A row without an id is named by its place among the holes, from 1. The
// three header lines are the first three lines that are not comments; after
// them blank lines are skipped. Line ends may be LF or CRLF.

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"

namespace pychron::laser {

struct Hole {
  std::string id;
  double x = 0;  // mm, tray frame
  double y = 0;
  double dimension = 0;  // mm: diameter of a circle, side of a square
};

enum class HoleShape { Circle, Square };

class TrayMap {
 public:
  // Config error "<name>:<line>: <what>" for anything that does not parse.
  static Result<TrayMap> parse(std::string_view text, std::string name);
  // The map's name is the file's stem.
  static Result<TrayMap> load(const std::filesystem::path& file);

  const std::string& name() const noexcept { return name_; }
  HoleShape shape() const noexcept { return shape_; }
  double dimension() const noexcept { return dimension_; }
  const std::vector<Hole>& holes() const noexcept { return holes_; }  // file order
  const Hole* find(std::string_view id) const;
  // The header's calibration holes; nullopt when the map names none.
  std::optional<std::string> center_hole() const;
  std::optional<std::string> right_hole() const;  // the east hole
  // Lower-case hex SHA-256 of the bytes parsed: a calibration made against
  // one version of a map is not used with another.
  const std::string& sha256() const noexcept { return sha256_; }

 private:
  std::string name_;
  HoleShape shape_ = HoleShape::Circle;
  double dimension_ = 0;
  std::vector<Hole> holes_;
  std::map<std::string, std::size_t, std::less<>> index_;
  std::vector<std::string> calibration_;  // empty, or north east south west centre
  std::string sha256_;
};

// The maps of a directory (<lab>/tray_maps/*.txt) by name.
class TrayLibrary {
 public:
  TrayLibrary() = default;
  // Never fails: a missing directory is an empty library, and a file that
  // does not load is reported in problems().
  static TrayLibrary load(const std::filesystem::path& dir);

  const TrayMap* find(std::string_view name) const;
  std::vector<std::string> names() const;  // sorted
  const std::vector<std::string>& problems() const noexcept { return problems_; }

 private:
  std::map<std::string, TrayMap, std::less<>> maps_;
  std::vector<std::string> problems_;
};

}  // namespace pychron::laser
