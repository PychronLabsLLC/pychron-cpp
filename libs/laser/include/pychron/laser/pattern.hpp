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
#include <memory>
#include <mutex>
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
  Trough,
  // Not a path: the stage follows the glowing sample for a time, by eye
  // (vision::Dragonfly). Needs a camera; run by PatternRunner like the rest.
  Dragonfly
};

// "polygon", "linear", "circular_contour", "line_spiral", "square_spiral",
// "random", "rubberband", "raster", "trough", "dragonfly".
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
//   dragonfly         duration (the run's, when it has one, is used instead)
//                     perimeter_radius saturation_threshold
//                     aggressiveness move_threshold max_step spiral spiral_base
//                     target_radius; velocity is the speed of its moves, and
//                     it has no iterations
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

  // dragonfly (legacy's defaults)
  double duration_s = 0;               // how long it follows the glow when the run does not say; 0: only the run's
  double perimeter_radius = 2.5;       // mm: never further from where it started
  double saturation_threshold = 0.75;  // a glow this bright: hold still
  double aggressiveness = 1.0;
  double move_threshold = 0.033;       // mm: smaller corrections are not made
  double max_step = 0.5;               // mm
  double spiral_base = 0.5;            // mm: the search when the glow is lost
  double target_radius = 0.5;          // mm: sizes the look
  bool square_spiral = false;          // spiral = "square"; else hexagon

  // A dragonfly: it has no path, and is run by eye.
  bool follows_glow() const noexcept { return kind == PatternKind::Dragonfly; }

  friend bool operator==(const Pattern&, const Pattern&) = default;

  // What a file holding only `kind` gives: legacy's defaults for the kind.
  static Pattern defaults(PatternKind kind);
  // Config error "<name>: <key>: <what>" for an unknown key, a key of
  // another kind, a wrong type, a value out of range, or a pattern of more
  // than kMaxPatternPoints points over its iterations: whatever could not
  // run is said when the file is read, not when the beam is on.
  static Result<Pattern> parse(std::string_view toml, std::string name);
  static Result<Pattern> load(const std::filesystem::path& file);  // the name is the stem
};

// The file a pattern is: `kind`, `velocity`, `iterations` (a dragonfly has
// none) and the kind's own keys, so that Pattern::parse(to_toml(p), p.name)
// gives p back. A dragonfly's duration is written only when it has one.
std::string to_toml(const Pattern& pattern);

// Writes <dir>/<name>.toml (the directory is made). Config error, and
// nothing written, for a name that cannot be a pattern file's (empty, a
// path, a leading dot) or a pattern that does not parse back: one that could
// not run is not saved. Written beside the target and renamed into place.
Result<std::filesystem::path> save_pattern(const std::filesystem::path& dir, const Pattern& pattern);

// One editable key of a kind of pattern, for whatever shows a form of them:
// velocity, iterations (not a dragonfly), then the kind's own, in the order
// the files list them.
struct PatternField {
  enum class Type { Number, Whole, Flag };
  std::string_view key;
  Type type = Type::Number;
  double low = 0;
  double high = 0;         // 1e6 or more: no upper limit worth showing
  bool above_low = false;  // low itself is not allowed
};
std::vector<PatternField> pattern_fields(PatternKind kind);
// nullopt / false for a key the pattern's kind does not have. A flag is 0 or
// 1; a whole number is rounded. The range is not checked here (parse does).
std::optional<double> field_value(const Pattern& pattern, std::string_view key);
bool set_field(Pattern& pattern, std::string_view key, double value);

// One pass of the pattern: offsets from its centre. `seed` is used by a
// random walk only, and the same seed gives the same walk on every machine.
std::vector<StageXY> pattern_points(const Pattern& pattern, std::uint64_t seed);

// How many points pattern_points() gives, without making them.
std::size_t pattern_point_count(const Pattern& pattern);

// The length of the path from the centre through `points`, mm.
double path_length(std::span<const StageXY> points);

// The whole path a runner follows: every iteration's points, then the centre
// again. A random walk is new on each iteration. Config error for more than
// kMaxPatternPoints, and for a pattern that follows the glow (it has no path).
Result<std::vector<StageXY>> pattern_path(const Pattern& pattern, std::uint64_t seed);

// The patterns of a directory (<lab>/patterns/*.toml) by name. It may be
// read from one thread while another puts a pattern in it: a reader keeps
// the pattern it found, whatever replaces it.
class PatternLibrary {
 public:
  PatternLibrary() = default;
  PatternLibrary(PatternLibrary&& other) noexcept;
  PatternLibrary& operator=(PatternLibrary&& other) noexcept;
  // Never fails: a missing directory is an empty library, and a file that
  // does not load is reported in problems() ("<name>: ...").
  static PatternLibrary load(const std::filesystem::path& dir);

  std::shared_ptr<const Pattern> find(std::string_view name) const;  // null: none of that name
  std::vector<std::string> names() const;  // sorted
  std::vector<std::string> problems() const;
  // Adds the pattern, or replaces the one of its name; a problem a file of
  // that name had is forgotten.
  void put(Pattern pattern);

 private:
  mutable std::mutex mutex_;
  std::map<std::string, std::shared_ptr<const Pattern>, std::less<>> patterns_;
  std::vector<std::string> problems_;
};

}  // namespace pychron::laser
