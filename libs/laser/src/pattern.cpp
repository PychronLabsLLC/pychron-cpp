#include "pychron/laser/pattern.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <numbers>
#include <random>
#include <sstream>
#include <system_error>
#include <variant>

#include <toml++/toml.hpp>

namespace pychron::laser {

namespace {

namespace fs = std::filesystem;

constexpr double kMaxMm = 1000;  // nothing on a sample stage is larger

constexpr std::array kKindNames{
    std::pair<std::string_view, PatternKind>{"polygon", PatternKind::Polygon},
    std::pair<std::string_view, PatternKind>{"linear", PatternKind::Linear},
    std::pair<std::string_view, PatternKind>{"circular_contour", PatternKind::CircularContour},
    std::pair<std::string_view, PatternKind>{"line_spiral", PatternKind::LineSpiral},
    std::pair<std::string_view, PatternKind>{"square_spiral", PatternKind::SquareSpiral},
    std::pair<std::string_view, PatternKind>{"random", PatternKind::Random},
    std::pair<std::string_view, PatternKind>{"rubberband", PatternKind::Rubberband},
    std::pair<std::string_view, PatternKind>{"raster", PatternKind::Raster},
    std::pair<std::string_view, PatternKind>{"trough", PatternKind::Trough},
    std::pair<std::string_view, PatternKind>{"dragonfly", PatternKind::Dragonfly},
};

// One key of a pattern file: where it goes and what it may be.
struct Key {
  std::string_view name;
  std::variant<double Pattern::*, int Pattern::*, bool Pattern::*> field;
  double low = 0;
  double high = 0;
  bool above_low = false;  // low itself is not allowed
};

constexpr double kAnyAngle = 1e6;

const Key kVelocity{"velocity", &Pattern::velocity, 0, kMaxMm, true};
const Key kIterations{"iterations", &Pattern::iterations, 1, 200};
const Key kRadius{"radius", &Pattern::radius, 0, kMaxMm, true};
const Key kRotation{"rotation", &Pattern::rotation, -kAnyAngle, kAnyAngle};
const Key kLength{"length", &Pattern::length, 0, kMaxMm, true};
const Key kWidth{"width", &Pattern::width, 0, kMaxMm, true};
const Key kOffset{"offset", &Pattern::offset, 0, kMaxMm};
const Key kDx{"dx", &Pattern::dx, 0, kMaxMm, true};
const Key kPercentChange{"percent_change", &Pattern::percent_change, 0, 100, true};
const Key kWalkX{"walk_x", &Pattern::walk_x, 0, kMaxMm, true};
const Key kWalkY{"walk_y", &Pattern::walk_y, 0, kMaxMm, true};
const Key kNsides{"nsides", &Pattern::nsides, 3, 200};
const Key kNpasses{"npasses", &Pattern::npasses, 1, 100};
const Key kNsteps{"nsteps", &Pattern::nsteps, 1, 10};
const Key kStepScalar{"step_scalar", &Pattern::step_scalar, 1, 20};
const Key kNpoints{"npoints", &Pattern::npoints, 1, 50};
const Key kDuration{"duration", &Pattern::duration_s, 0, 3600, true};
const Key kPerimeter{"perimeter_radius", &Pattern::perimeter_radius, 0, kMaxMm, true};
const Key kSaturation{"saturation_threshold", &Pattern::saturation_threshold, 0, 1, true};
const Key kAggressiveness{"aggressiveness", &Pattern::aggressiveness, 0, 10};
const Key kMoveThreshold{"move_threshold", &Pattern::move_threshold, 0, kMaxMm};
const Key kMaxStep{"max_step", &Pattern::max_step, 0, kMaxMm, true};
const Key kSpiralBase{"spiral_base", &Pattern::spiral_base, 0, kMaxMm, true};
const Key kTargetRadius{"target_radius", &Pattern::target_radius, 0, kMaxMm, true};
const Key kSinglePass{"single_pass", &Pattern::single_pass};
const Key kUseX{"use_x", &Pattern::use_x};

std::vector<const Key*> keys_of(PatternKind kind) {
  switch (kind) {
    case PatternKind::Polygon: return {&kRadius, &kNsides, &kRotation};
    case PatternKind::Linear: return {&kLength, &kRotation, &kNpasses};
    case PatternKind::CircularContour: return {&kRadius, &kNsteps, &kPercentChange};
    case PatternKind::LineSpiral: return {&kRadius, &kNsteps, &kPercentChange, &kStepScalar};
    case PatternKind::SquareSpiral: return {&kRadius, &kNsteps, &kPercentChange};
    case PatternKind::Random: return {&kWalkX, &kWalkY, &kNpoints};
    case PatternKind::Rubberband: return {&kLength, &kOffset, &kRotation};
    case PatternKind::Raster: return {&kLength, &kOffset, &kRotation, &kDx, &kSinglePass};
    case PatternKind::Trough: return {&kLength, &kWidth, &kRotation, &kUseX};
    case PatternKind::Dragonfly:
      return {&kDuration, &kPerimeter, &kSaturation, &kAggressiveness, &kMoveThreshold, &kMaxStep, &kSpiralBase,
              &kTargetRadius};
  }
  return {};
}

std::string plain(double value) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << value;
  return out.str();
}

// Why `node` cannot be the value of `key`; empty when it was stored.
std::string store(const Key& key, const toml::node& node, Pattern& pattern) {
  if (const auto* field = std::get_if<bool Pattern::*>(&key.field)) {
    const auto* b = node.as_boolean();
    if (b == nullptr) return "expected true or false";
    pattern.**field = b->get();
    return {};
  }
  const std::string range = std::string(key.above_low ? "above " : "from ") + plain(key.low) +
                            (key.high >= kAnyAngle ? "" : " to " + plain(key.high));
  if (const auto* field = std::get_if<int Pattern::*>(&key.field)) {
    const auto* i = node.as_integer();
    if (i == nullptr) return "expected a whole number";
    if (static_cast<double>(i->get()) < key.low || static_cast<double>(i->get()) > key.high) {
      return "must be " + range;
    }
    pattern.**field = static_cast<int>(i->get());
    return {};
  }
  const auto* field = std::get_if<double Pattern::*>(&key.field);
  double value = 0;
  if (const auto* f = node.as_floating_point()) value = f->get();
  else if (const auto* i = node.as_integer()) value = static_cast<double>(i->get());
  else return "expected a number";
  if (!std::isfinite(value)) return "must be a finite number";
  if (value < key.low || value > key.high || (key.above_low && value <= key.low)) return "must be " + range;
  pattern.**field = value;
  return {};
}

// How many steps a raster takes across its box. Legacy fits the step to the
// box: an even count of the steps asked for, plus one, so the zig-zag ends on
// the far edge. In double, and bounded: a step of 1e-300 is not a count an
// int can hold.
std::size_t raster_steps(const Pattern& p) {
  const double total = p.length + 2 * p.offset;
  double n = std::floor(total / p.dx + 1e-9);
  if (!(n < 4.0 * static_cast<double>(kMaxPatternPoints))) return 4 * kMaxPatternPoints;  // also NaN and inf
  n = std::max<double>(n, 0);
  if (std::fmod(n, 2.0) != 0) n += 1;
  return static_cast<std::size_t>(n) + 1;
}

StageXY turned(double x, double y, double degrees) {
  const double a = degrees * std::numbers::pi / 180.0;
  return {x * std::cos(a) - y * std::sin(a), x * std::sin(a) + y * std::cos(a)};
}

StageXY on_circle(double radius, double degrees) {
  const double a = degrees * std::numbers::pi / 180.0;
  return {radius * std::cos(a), radius * std::sin(a)};
}

}  // namespace

std::string_view to_string(PatternKind kind) noexcept {
  for (const auto& [name, k] : kKindNames) {
    if (k == kind) return name;
  }
  return "polygon";
}

Pattern Pattern::defaults(PatternKind kind) {
  Pattern p;
  p.kind = kind;
  switch (kind) {
    case PatternKind::CircularContour:
    case PatternKind::LineSpiral:
    case PatternKind::SquareSpiral: p.radius = 0.1; break;
    case PatternKind::Rubberband:
    case PatternKind::Raster: p.length = 15; break;
    case PatternKind::Trough: p.length = 10; break;
    default: break;
  }
  return p;
}

Result<Pattern> Pattern::parse(std::string_view text, std::string name) {
  const auto bad = [&name](std::string_view key, const std::string& what) {
    return fail(ErrorKind::Config, name + ": " + (key.empty() ? "" : std::string(key) + ": ") + what);
  };
  const toml::parse_result parsed = toml::parse(text);
  if (!parsed) return bad("", std::string(parsed.error().description()));
  const toml::table& table = parsed.table();

  const toml::node* kind_node = table.get("kind");
  if (kind_node == nullptr) return bad("kind", "missing (what the pattern is)");
  const auto kind_name = kind_node->value<std::string>();
  std::optional<PatternKind> kind;
  if (kind_name) {
    for (const auto& [n, k] : kKindNames) {
      if (n == *kind_name) kind = k;
    }
  }
  if (!kind) {
    std::string known;
    for (const auto& [n, k] : kKindNames) known += (known.empty() ? "" : ", ") + std::string(n);
    return bad("kind", "expected one of " + known);
  }

  Pattern p = defaults(*kind);
  p.name = std::move(name);
  std::vector<const Key*> keys = keys_of(*kind);
  keys.push_back(&kVelocity);
  if (*kind != PatternKind::Dragonfly) keys.push_back(&kIterations);  // a dragonfly runs once, for its duration
  for (const auto& [k, node] : table) {
    const std::string_view key = k.str();
    if (key == "kind") continue;
    if (key == "seed" && *kind == PatternKind::Random) {
      const auto* i = node.as_integer();
      if (i == nullptr || i->get() < 0) return fail(ErrorKind::Config, p.name + ": seed: expected a whole number, 0 or more");
      p.seed = static_cast<std::uint64_t>(i->get());
      continue;
    }
    if (key == "spiral" && *kind == PatternKind::Dragonfly) {
      const auto which = node.value<std::string>();
      if (!which || (*which != "hexagon" && *which != "square")) {
        return fail(ErrorKind::Config, p.name + ": spiral: expected hexagon or square");
      }
      p.square_spiral = *which == "square";
      continue;
    }
    const auto found = std::find_if(keys.begin(), keys.end(), [key](const Key* candidate) { return candidate->name == key; });
    if (found == keys.end()) {
      return fail(ErrorKind::Config, p.name + ": " + std::string(key) + ": not a key of a " +
                                         std::string(to_string(*kind)) + " pattern");
    }
    if (const std::string why = store(**found, node, p); !why.empty()) {
      return fail(ErrorKind::Config, p.name + ": " + std::string(key) + ": " + why);
    }
  }
  if (*kind == PatternKind::Raster && p.dx > p.length + 2 * p.offset) {
    return fail(ErrorKind::Config, p.name + ": dx: the step is wider than the box it rasters (length + 2 offset)");
  }
  if (*kind == PatternKind::Raster && raster_steps(p) > kMaxPatternPoints) {
    return fail(ErrorKind::Config, p.name + ": dx: the step is so fine the raster has more than " +
                                       std::to_string(kMaxPatternPoints) + " points");
  }
  // Every iteration's points, and the return to the center.
  const std::size_t points = pattern_point_count(p) * static_cast<std::size_t>(p.iterations) + 1;
  if (points > kMaxPatternPoints) {
    return fail(ErrorKind::Config, p.name + ": iterations: the pattern has " + std::to_string(points) +
                                       " points over its iterations; at most " + std::to_string(kMaxPatternPoints));
  }
  return p;
}

Result<Pattern> Pattern::load(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return fail(ErrorKind::Config, file.stem().string() + ": cannot read " + file.string());
  std::ostringstream text;
  text << in.rdbuf();
  return parse(text.str(), file.stem().string());
}

std::size_t pattern_point_count(const Pattern& p) {
  const auto n = [](int value) { return static_cast<std::size_t>(std::max(value, 0)); };
  switch (p.kind) {
    case PatternKind::Polygon: return n(p.nsides) + 1;
    case PatternKind::Linear: return 2 * n(p.npasses);
    case PatternKind::CircularContour: return 37 * n(p.nsteps);
    case PatternKind::LineSpiral: {
      std::size_t count = 0;
      for (int turn = 0; turn < p.nsteps; ++turn) {
        const int angles = 2 * turn + p.step_scalar;
        if (angles <= 1) continue;  // one angle is both 0 and 360
        count += n(angles) - (turn != p.nsteps - 1 ? 1 : 0);
      }
      return std::max<std::size_t>(count, 1);
    }
    case PatternKind::SquareSpiral: return 4 * n(p.nsteps) + 1;
    case PatternKind::Random: return n(p.npoints);
    case PatternKind::Rubberband: return 5;
    case PatternKind::Raster: return p.single_pass ? raster_steps(p) + 1 : 2 * (raster_steps(p) + 1) + 1;
    case PatternKind::Trough: return 5;
    case PatternKind::Dragonfly: return 0;  // no path
  }
  return 0;
}

std::vector<StageXY> pattern_points(const Pattern& p, std::uint64_t seed) {
  std::vector<StageXY> out;
  switch (p.kind) {
    case PatternKind::Polygon:
      for (int i = 0; i <= p.nsides; ++i) out.push_back(on_circle(p.radius, 360.0 * (i % p.nsides) / p.nsides + p.rotation));
      break;
    case PatternKind::Linear: {
      const StageXY p1{0, 0};
      const StageXY p2 = turned(p.length, 0, p.rotation);
      for (int i = 0; i < p.npasses; ++i) {
        out.push_back(i % 2 == 0 ? p1 : p2);
        out.push_back(i % 2 == 0 ? p2 : p1);
      }
      break;
    }
    case PatternKind::CircularContour:
      for (int ring = 0; ring < p.nsteps; ++ring) {
        const double r = p.radius * (1 + ring * p.percent_change);
        for (int step = 0; step <= 36; ++step) out.push_back(on_circle(r, 10.0 * (step % 36)));
      }
      break;
    case PatternKind::LineSpiral:
      for (int turn = 0; turn < p.nsteps; ++turn) {
        const int n = 2 * turn + p.step_scalar;
        for (int j = 0; j < n; ++j) {
          const bool full = j == n - 1;  // the turn's last angle is 360
          if (n == 1 || (full && turn != p.nsteps - 1)) continue;
          const double t = 360.0 * j / (n - 1);
          const double r = p.radius * (1 + (turn + t / 360.0) * p.percent_change);
          out.push_back(on_circle(r, full ? 0 : t));
        }
      }
      // A step_scalar of 1 on the first turn gives it no angles; the spiral
      // then starts where it would have.
      if (out.empty()) out.push_back({p.radius, 0});
      break;
    case PatternKind::SquareSpiral: {
      double x = 0, y = 0;
      for (int i = 0; i < 4 * p.nsteps + 1; ++i) {
        const double r = p.radius * (1 + i * p.percent_change);
        switch (i % 4) {
          case 0: x += r; break;
          case 1: y += r; break;
          case 2: x -= r; break;
          default: y -= r; break;
        }
        out.push_back({x, y});
      }
      break;
    }
    case PatternKind::Random: {
      // Spelled out, not std::uniform_real_distribution: the same seed gives
      // the same walk under every standard library.
      std::mt19937_64 rng(seed);
      const auto unit = [&rng] { return static_cast<double>(rng() >> 11) * 0x1.0p-53; };
      for (int i = 0; i < p.npoints; ++i) {
        double x = 0, y = 0;
        // A point in the box, within walk_x of the center (legacy's test). No
        // point further than walk_x in y can pass it, so y is drawn only
        // that far: the same walk, and a tall narrow box still ends.
        const double reach_y = std::min(p.walk_y, p.walk_x);
        do {
          x = (unit() * 2 - 1) * p.walk_x;
          y = (unit() * 2 - 1) * reach_y;
        } while (std::hypot(x, y) > p.walk_x);
        out.push_back({x, y});
      }
      break;
    }
    case PatternKind::Rubberband: {
      const double o = p.offset, far = p.length + p.offset;
      for (const auto& [x, y] : {std::pair{-o, o}, std::pair{far, o}, std::pair{far, -o}, std::pair{-o, -o}, std::pair{-o, o}}) {
        out.push_back(turned(x, y, p.rotation));
      }
      break;
    }
    case PatternKind::Raster: {
      const double o = p.offset, total = p.length + 2 * p.offset;
      const std::size_t steps = raster_steps(p);  // bounded, whatever dx is
      const double dx = total / static_cast<double>(steps);
      const auto across = [dx](std::size_t i) { return dx * static_cast<double>(i); };
      for (std::size_t i = 0; i <= steps; ++i) out.push_back(turned(-o + across(i), i % 2 == 0 ? o : -o, p.rotation));
      if (!p.single_pass) {
        for (std::size_t i = 0; i <= steps; ++i) {
          out.push_back(turned(p.length + o - across(i), i % 2 == 0 ? o : -o, p.rotation));
        }
        out.push_back(turned(-o, o, p.rotation));
      }
      break;
    }
    case PatternKind::Trough: {
      const StageXY p1{0, 0};
      const StageXY p2 = turned(p.length, 0, p.rotation);
      const StageXY p3 = turned(p.length, -p.width, p.rotation);
      const StageXY p4 = turned(0, -p.width, p.rotation);
      if (p.use_x) out = {p1, p2, p4, p3, p1};
      else out = {p1, p2, p3, p4, p1};
      break;
    }
    case PatternKind::Dragonfly: break;  // where it goes is decided by eye, as it runs
  }
  return out;
}

double path_length(std::span<const StageXY> points) {
  double length = 0;
  StageXY at{0, 0};
  for (const auto& p : points) {
    length += std::hypot(p.x - at.x, p.y - at.y);
    at = p;
  }
  return length;
}

Result<std::vector<StageXY>> pattern_path(const Pattern& pattern, std::uint64_t seed) {
  if (pattern.follows_glow()) {
    return fail(ErrorKind::Config, "pattern " + pattern.name + " follows the glow: it has no path");
  }
  std::vector<StageXY> path;
  for (int i = 0; i < pattern.iterations; ++i) {
    const auto points = pattern_points(pattern, seed + static_cast<std::uint64_t>(i));
    if (path.size() + points.size() + 1 > kMaxPatternPoints) {
      return fail(ErrorKind::Config, "pattern " + pattern.name + " has more than " + std::to_string(kMaxPatternPoints) +
                                         " points over its " + std::to_string(pattern.iterations) + " iterations");
    }
    path.insert(path.end(), points.begin(), points.end());
  }
  path.push_back({0, 0});
  return path;
}

namespace {

std::vector<const Key*> all_keys_of(PatternKind kind) {
  std::vector<const Key*> keys{&kVelocity};
  if (kind != PatternKind::Dragonfly) keys.push_back(&kIterations);  // a dragonfly runs once, for its duration
  for (const Key* key : keys_of(kind)) keys.push_back(key);
  return keys;
}

const Key* key_of(PatternKind kind, std::string_view name) {
  for (const Key* key : all_keys_of(kind)) {
    if (key->name == name) return key;
  }
  return nullptr;
}

// The shortest text that reads back as exactly `value`.
std::string exact(double value) {
  char buffer[32];
  const auto end = std::to_chars(buffer, buffer + sizeof buffer, value).ptr;
  return std::string(buffer, end);
}

}  // namespace

std::vector<PatternField> pattern_fields(PatternKind kind) {
  std::vector<PatternField> out;
  for (const Key* key : all_keys_of(kind)) {
    PatternField field;
    field.key = key->name;
    field.type = std::holds_alternative<bool Pattern::*>(key->field)  ? PatternField::Type::Flag
                 : std::holds_alternative<int Pattern::*>(key->field) ? PatternField::Type::Whole
                                                                      : PatternField::Type::Number;
    field.low = key->low;
    field.high = key->high;
    field.above_low = key->above_low;
    out.push_back(field);
  }
  return out;
}

std::optional<double> field_value(const Pattern& pattern, std::string_view name) {
  const Key* key = key_of(pattern.kind, name);
  if (key == nullptr) return std::nullopt;
  if (const auto* f = std::get_if<bool Pattern::*>(&key->field)) return pattern.**f ? 1.0 : 0.0;
  if (const auto* f = std::get_if<int Pattern::*>(&key->field)) return static_cast<double>(pattern.**f);
  return pattern.**std::get_if<double Pattern::*>(&key->field);
}

bool set_field(Pattern& pattern, std::string_view name, double value) {
  const Key* key = key_of(pattern.kind, name);
  if (key == nullptr) return false;
  if (const auto* f = std::get_if<bool Pattern::*>(&key->field)) {
    pattern.**f = value != 0;
  } else if (const auto* i = std::get_if<int Pattern::*>(&key->field)) {
    // Out of an int's reach is out of every key's range: parse says so.
    const double whole = std::isfinite(value) ? std::round(std::clamp(value, -2.0e9, 2.0e9)) : 0.0;
    pattern.**i = static_cast<int>(whole);
  } else {
    pattern.**std::get_if<double Pattern::*>(&key->field) = value;
  }
  return true;
}

std::string to_toml(const Pattern& pattern) {
  std::string out = "kind = \"" + std::string(to_string(pattern.kind)) + "\"\n";
  for (const Key* key : all_keys_of(pattern.kind)) {
    // No duration of its own: it runs for as long as the run says.
    if (key == &kDuration && !(pattern.duration_s > 0)) continue;
    out += std::string(key->name) + " = ";
    if (const auto* f = std::get_if<bool Pattern::*>(&key->field)) out += pattern.**f ? "true" : "false";
    else if (const auto* i = std::get_if<int Pattern::*>(&key->field)) out += std::to_string(pattern.**i);
    else out += exact(pattern.**std::get_if<double Pattern::*>(&key->field));
    out += '\n';
  }
  if (pattern.kind == PatternKind::Random && pattern.seed) out += "seed = " + std::to_string(*pattern.seed) + "\n";
  if (pattern.kind == PatternKind::Dragonfly) {
    out += std::string("spiral = \"") + (pattern.square_spiral ? "square" : "hexagon") + "\"\n";
  }
  return out;
}

Result<void> check_pattern(const Pattern& pattern) {
  const std::string& name = pattern.name;
  if (name.empty()) return fail(ErrorKind::Config, "a pattern needs a name");
  // A name that is one plain part of a file name; a leading dot would be a
  // file the library does not read.
  if (name == "." || name == ".." || name.front() == '.' || name.find_first_of("/\\") != std::string::npos ||
      name.find('\0') != std::string::npos) {
    return fail(ErrorKind::Config, "'" + name + "' cannot be a pattern's name (it names the file)");
  }
  // What could not run is not saved: the same check a file gets when read.
  auto back = Pattern::parse(to_toml(pattern), name);
  if (!back) return fail(back.error());
  return {};
}

Result<fs::path> save_pattern(const fs::path& dir, const Pattern& pattern) {
  if (auto ok = check_pattern(pattern); !ok) return fail(ok.error());
  const std::string& name = pattern.name;
  const std::string text = to_toml(pattern);
  const fs::path path = dir / (name + ".toml");
  const auto cannot = [&path](const std::string& why) {
    return fail(ErrorKind::Io, "cannot write " + path.string() + (why.empty() ? "" : ": " + why));
  };
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) return cannot(ec.message());
  // Beside the target, then renamed: a reader never sees a torn file.
  const fs::path tmp = fs::path(path).concat(".tmp");
  {
    std::ofstream out(tmp, std::ios::out | std::ios::trunc | std::ios::binary);
    if (out) out << text;
    out.flush();
    if (!out) {
      fs::remove(tmp, ec);
      return cannot("");
    }
  }
  fs::rename(tmp, path, ec);
  if (ec) {
    const std::string why = ec.message();
    fs::remove(tmp, ec);
    return cannot(why);
  }
  return path;
}

PatternLibrary::PatternLibrary(PatternLibrary&& other) noexcept {
  std::lock_guard lock(other.mutex_);
  patterns_ = std::move(other.patterns_);
  problems_ = std::move(other.problems_);
}

PatternLibrary& PatternLibrary::operator=(PatternLibrary&& other) noexcept {
  if (this != &other) {
    std::scoped_lock lock(mutex_, other.mutex_);
    patterns_ = std::move(other.patterns_);
    problems_ = std::move(other.problems_);
  }
  return *this;
}

PatternLibrary PatternLibrary::load(const fs::path& dir) {
  PatternLibrary lib;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return lib;
  std::vector<fs::path> files;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const fs::path& path = it->path();
    if (path.extension() != ".toml" || path.filename().string().starts_with('.')) continue;
    std::error_code type;
    if (fs::is_regular_file(path, type)) files.push_back(path);
  }
  std::sort(files.begin(), files.end());  // directory order is unspecified
  for (const auto& file : files) {
    auto pattern = Pattern::load(file);
    if (!pattern) {
      lib.problems_.push_back(pattern.error().what);
      continue;
    }
    std::string name = pattern->name;
    lib.patterns_.insert_or_assign(std::move(name), std::make_shared<const Pattern>(std::move(*pattern)));
  }
  return lib;
}

std::shared_ptr<const Pattern> PatternLibrary::find(std::string_view name) const {
  std::lock_guard lock(mutex_);
  const auto it = patterns_.find(name);
  return it == patterns_.end() ? nullptr : it->second;
}

std::vector<std::string> PatternLibrary::names() const {
  std::lock_guard lock(mutex_);
  std::vector<std::string> out;
  for (const auto& [name, pattern] : patterns_) out.push_back(name);
  return out;
}

std::vector<std::string> PatternLibrary::problems() const {
  std::lock_guard lock(mutex_);
  return problems_;
}

void PatternLibrary::put(Pattern pattern) {
  std::string name = pattern.name;
  auto made = std::make_shared<const Pattern>(std::move(pattern));
  std::lock_guard lock(mutex_);
  std::erase_if(problems_, [&name](const std::string& problem) { return problem.starts_with(name + ": "); });
  patterns_.insert_or_assign(std::move(name), std::move(made));
}

}  // namespace pychron::laser
