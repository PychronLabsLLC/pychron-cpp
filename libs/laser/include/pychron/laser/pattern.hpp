#pragma once

// Laser patterns: a path the beam is moved along over a sample while it
// heats (docs/superpowers/specs/2026-10-04-laser-patterns-design.md).
//
// A pattern is a file, <lab>/patterns/<name>.toml:
//
//   kind = "polygon"      polygon | linear | circular_contour | line_spiral |
//                         square_spiral | random | rubberband | raster | trough
//   velocity = 1.0        mm/s
//   iterations = 1        the whole pattern, repeated
//   radius = 0.5          the kind's own keys; legacy pychron's names and defaults
//   nsides = 6
//   rotation = 0
//
// and a pure function from that to points: offsets in millimetres from the
// pattern's centre, in the stage's axes, angles in degrees counter-clockwise.
// The geometry of each kind is legacy pychron's (pattern_generators.py).
// Running one is PatternRunner's (pattern_runner.hpp).

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/laser/calibration.hpp"

namespace pychron::laser {

enum class PatternKind {
  Polygon,
  Linear,
  CircularContour,
  LineSpiral,
  SquareSpiral,
  Random,
  Rubberband,
  Raster,
  Trough
};

// "polygon", "linear", "circular_contour", "line_spiral", "square_spiral",
// "random", "rubberband", "raster", "trough".
std::string_view to_string(PatternKind kind) noexcept;

// Most points a pattern may have over all its iterations.
inline constexpr std::size_t kMaxPatternPoints = 10000;

// One set of fields; a kind uses the ones its row lists and ignores the rest.
//
//   polygon           radius nsides rotation
//   linear            length rotation npasses
//   circular_contour  radius nsteps percent_change
//   line_spiral       radius nsteps percent_change step_scalar
//   square_spiral     radius nsteps percent_change
//   random            walk_x walk_y npoints seed
//   rubberband        length offset rotation
//   raster            length offset rotation dx single_pass
//   trough            length width rotation use_x
struct Pattern {
  std::string name;
  PatternKind kind = PatternKind::Polygon;
  double velocity = 1.0;  // mm/s
  int iterations = 1;

  double radius = 0.5;
  double rotation = 0;
  double length = 1;
  double width = 10;
  double offset = 0;
  double dx = 0.5;
  double percent_change = 0.8;
  double walk_x = 1;
  double walk_y = 1;
  int nsides = 6;
  int npasses = 1;
  int nsteps = 2;
  int step_scalar = 5;
  int npoints = 10;
  bool single_pass = true;
  bool use_x = true;
  std::optional<std::uint64_t> seed;  // random: unset, a new walk each run

  friend bool operator==(const Pattern&, const Pattern&) = default;

  // What a file holding only `kind` gives: legacy's defaults for the kind.
  static Pattern defaults(PatternKind kind);
  // Config error "<name>: <key>: <what>" for an unknown key, a key of
  // another kind, a wrong type or a value out of range.
  static Result<Pattern> parse(std::string_view toml, std::string name);
  static Result<Pattern> load(const std::filesystem::path& file);  // the name is the stem
};

// One pass of the pattern: offsets from its centre. `seed` is used by a
// random walk only, and the same seed gives the same walk on every machine.
std::vector<StageXY> pattern_points(const Pattern& pattern, std::uint64_t seed);

// The length of the path from the centre through `points`, mm.
double path_length(std::span<const StageXY> points);

// The whole path a runner follows: every iteration's points, then the centre
// again. A random walk is new on each iteration. Config error for more than
// kMaxPatternPoints.
Result<std::vector<StageXY>> pattern_path(const Pattern& pattern, std::uint64_t seed);

// The patterns of a directory (<lab>/patterns/*.toml) by name.
class PatternLibrary {
 public:
  PatternLibrary() = default;
  // Never fails: a missing directory is an empty library, and a file that
  // does not load is reported in problems() ("<name>: ...").
  static PatternLibrary load(const std::filesystem::path& dir);

  const Pattern* find(std::string_view name) const;
  std::vector<std::string> names() const;  // sorted
  const std::vector<std::string>& problems() const noexcept { return problems_; }

 private:
  std::map<std::string, Pattern, std::less<>> patterns_;
  std::vector<std::string> problems_;
};

}  // namespace pychron::laser
