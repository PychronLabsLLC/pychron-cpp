#include "pychron/laser/track_bench.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <locale>
#include <random>
#include <sstream>
#include <system_error>
#include <utility>

#include <toml++/toml.hpp>

#include "pychron/core/clock.hpp"
#include "pychron/core/user_file.hpp"
#include "pychron/devices/extraction/chromium.hpp"
#include "pychron/devices/extraction/chromium_sim.hpp"
#include "pychron/laser/calibration_store.hpp"
#include "pychron/laser/camera.hpp"
#include "pychron/laser/correction_store.hpp"
#include "pychron/laser/laser_system.hpp"
#include "pychron/laser/pattern.hpp"
#include "pychron/laser/tray_camera.hpp"
#include "pychron/laser/tray_map.hpp"
#include "pychron/transport/sim_transport.hpp"
#include "pychron/vision/png.hpp"

namespace pychron::laser {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {
constexpr int kMaxRuns = 10000;
constexpr double kDeadmanS = 30;     // past its duration a run is given up on
constexpr int kStagePolls = 600;     // 60 s simulated to reach the hole
constexpr auto kStagePoll = 100ms;
constexpr std::string_view kDevice = "co2";
constexpr std::string_view kTray = "track";
}  // namespace

// What a scenario says. `sections` are the tables a sweep may reach, in the
// order labels name them.
struct TrackTables {
  double output_percent = 20;
  double duration_s = 30;
  int seeds = 20;
  std::uint64_t seed = 1;
  int poll_ms = 100;
  std::array<double, 2> grain_offset_mm{};
  toml::table pattern, grain, camera;

  struct Sweep {
    std::string table, key;
    toml::array values;
  };
  std::vector<Sweep> sweeps;
};

namespace {

constexpr std::array<std::string_view, 3> kSwept{"pattern", "grain", "camera"};
constexpr std::array<std::string_view, 6> kCameraKeys{"settle_ms", "frames_per_step", "noise",
                                                      "px_per_mm", "width",           "height"};

std::optional<double> number_of(const toml::node& node) {
  if (const auto* f = node.as_floating_point()) return f->get();
  if (const auto* i = node.as_integer()) return static_cast<double>(i->get());
  return std::nullopt;
}

// What is wrong with a key a [pattern] or [camera] table, or its sweep, may
// not have; empty: nothing. [grain]'s keys are all the real loader's.
std::string_view refused_key(std::string_view table, std::string_view key) {
  if (table == "pattern" && (key == "kind" || key == "duration")) {
    return "the bench sets it: a dragonfly, for the scenario's duration";
  }
  if (table == "camera" && std::find(kCameraKeys.begin(), kCameraKeys.end(), key) == kCameraKeys.end()) {
    return "unknown key";
  }
  return {};
}

std::string read_sweeps(const toml::table& sweep, TrackTables& t) {
  for (const auto& [k, node] : sweep) {
    if (std::find(kSwept.begin(), kSwept.end(), k.str()) == kSwept.end()) {
      return "sweep." + std::string(k.str()) + ": nothing to sweep there (pattern, grain or camera)";
    }
    if (node.as_table() == nullptr) return "sweep." + std::string(k.str()) + ": expected a table";
  }
  for (const std::string_view name : kSwept) {
    const toml::table* entries = sweep[name].as_table();
    if (entries == nullptr) continue;
    for (const auto& [k, node] : *entries) {
      std::string where = "sweep.";
      where.append(name).append(".").append(k.str());
      if (const std::string_view why = refused_key(name, k.str()); !why.empty()) return where.append(": ").append(why);
      const auto* values = node.as_array();
      if (values == nullptr) return where + ": expected a list of values";
      if (values->empty()) return where + ": an empty list sweeps nothing";
      t.sweeps.push_back({std::string(name), std::string(k.str()), *values});
    }
  }
  return {};
}

std::string read_table(std::string_view key, const toml::node& node, toml::table& out) {
  const auto* table = node.as_table();
  if (table == nullptr) return std::string(key) + ": expected a table";
  for (const auto& [k, value] : *table) {
    if (const std::string_view why = refused_key(key, k.str()); !why.empty()) {
      return std::string(key).append(".").append(k.str()).append(": ").append(why);
    }
  }
  out = *table;
  return {};
}

// The first thing wrong, as "<key>: <what>"; empty: nothing.
std::string read_scenario(const toml::table& root, TrackTables& t) {
  for (const auto& [k, node] : root) {
    const std::string key(k.str());
    const auto value = number_of(node);
    if (key == "output_percent") {
      if (!value || !(*value > 0 && *value <= 100)) return key + ": must be above 0, to 100";
      t.output_percent = *value;
    } else if (key == "duration") {
      if (!value || !(*value > 0 && *value <= 3600)) return key + ": must be above 0, to 3600 s";
      t.duration_s = *value;
    } else if (key == "seeds") {
      const auto* i = node.as_integer();
      if (i == nullptr || i->get() < 1 || i->get() > kMaxRuns) return key + ": must be a whole number from 1 to 10000";
      t.seeds = static_cast<int>(i->get());
    } else if (key == "seed") {
      const auto* i = node.as_integer();
      if (i == nullptr || i->get() < 0) return key + ": must be a whole number, 0 or more";
      t.seed = static_cast<std::uint64_t>(i->get());
    } else if (key == "poll_ms") {
      const auto* i = node.as_integer();
      if (i == nullptr || i->get() < 10 || i->get() > 1000) return key + ": must be a whole number from 10 to 1000";
      t.poll_ms = static_cast<int>(i->get());
    } else if (key == "grain_offset_mm") {
      const auto* pair = node.as_array();
      if (pair == nullptr || pair->size() != 2) return key + ": expected [x, y]";
      const auto x = number_of(*pair->get(0));
      const auto y = number_of(*pair->get(1));
      if (!x || !y) return key + ": expected [x, y]";
      t.grain_offset_mm = {*x, *y};
    } else if (key == "pattern" || key == "grain" || key == "camera") {
      toml::table& out = key == "pattern" ? t.pattern : key == "grain" ? t.grain : t.camera;
      if (std::string why = read_table(key, node, out); !why.empty()) return why;
    } else if (key == "sweep") {
      const auto* sweep = node.as_table();
      if (sweep == nullptr) return key + ": expected a table";
      if (std::string why = read_sweeps(*sweep, t); !why.empty()) return why;
    } else {
      return key + ": unknown key";
    }
  }
  return {};
}

// The text of a sweep's value in a label: the shortest that reads back.
std::string value_text(const toml::node& node) {
  if (const auto* f = node.as_floating_point()) {
    std::array<char, 32> buffer{};
    const auto end = std::to_chars(buffer.data(), buffer.data() + buffer.size(), f->get()).ptr;
    return {buffer.data(), end};
  }
  std::ostringstream out;
  out.imbue(std::locale::classic());
  node.visit([&out](const auto& value) { out << value; });
  return out.str();
}

std::string toml_text(const toml::table& table) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << table;
  return out.str();
}

struct Sections {
  toml::table pattern, grain, camera;
};

// One combination of the sweeps: what it is called and what the real loaders
// made of it.
struct Combination {
  std::string label;
  std::string pattern;  // its name in the library
  CameraConfig camera;
  double beam_radius_mm = 0;
  double full_heat = 0;
};

std::string cameras_text(const TrackTables& t, const Sections& s) {
  toml::table sim;
  sim.insert_or_assign("tray_error_mm", toml::array{0.0, 0.0});
  sim.insert_or_assign("grain_offset_mm", toml::array{t.grain_offset_mm[0], t.grain_offset_mm[1]});
  sim.insert_or_assign("grain", s.grain);  // an empty one too: the model is what the bench is for

  // A camera that fails is a failed run, not a stage held still and scored.
  toml::table autocenter;
  autocenter.insert_or_assign("on_failure", "fail");

  toml::table device;
  device.insert_or_assign("source", "sim");
  device.insert_or_assign("use", "center");
  for (const auto& [k, node] : s.camera) {
    const std::string_view key = k.str();
    if (key == "settle_ms" || key == "px_per_mm") device.insert_or_assign(key, node);
    else if (key == "frames_per_step") autocenter.insert_or_assign(key, node);
    else sim.insert_or_assign(key, node);
  }
  device.insert_or_assign("sim", std::move(sim));
  device.insert_or_assign("autocenter", std::move(autocenter));
  toml::table root;
  root.insert_or_assign(kDevice, std::move(device));
  return toml_text(root);
}

// Writes a file of the scratch directory; Io error when that cannot be done.
Result<void> put(const fs::path& file, std::string_view text) {
  std::ofstream out(file, std::ios::binary);
  out << text;
  out.flush();
  if (!out) return fail(ErrorKind::Io, "cannot write " + file.string());
  return {};
}

// A directory under temp_directory_path(), gone when this is, however the
// bench ended.
class Scratch {
 public:
  Scratch() {
    std::random_device rd;
    dir_ = fs::temp_directory_path(ec_) / ("pychron_track_" + std::to_string(rd()) + std::to_string(rd()));
    if (!ec_) fs::create_directories(dir_ / "tray_maps", ec_);
    if (!ec_) fs::create_directories(dir_ / "patterns", ec_);
  }
  Scratch(const Scratch&) = delete;
  Scratch& operator=(const Scratch&) = delete;
  Scratch(Scratch&&) = delete;
  Scratch& operator=(Scratch&&) = delete;
  ~Scratch() {
    std::error_code ignored;
    fs::remove_all(dir_, ignored);
  }
  const fs::path& dir() const noexcept { return dir_; }
  // Why it could not be made; empty: it was.
  std::string problem() const { return ec_ ? "cannot make " + dir_.string() + ": " + ec_.message() : ""; }

 private:
  fs::path dir_;
  std::error_code ec_;
};

// The tray, its calibration and the libraries every run shares.
struct Stock {
  TrayLibrary trays;
  PatternLibrary patterns;
  std::vector<Combination> combinations;
};

// Each combination's tables, the sweeps applied: first sweep slowest.
std::vector<std::pair<std::string, Sections>> expand(const TrackTables& t) {
  std::vector<std::pair<std::string, Sections>> out;
  std::vector<std::size_t> at(t.sweeps.size(), 0);
  while (true) {
    std::pair<std::string, Sections> one{"", Sections{t.pattern, t.grain, t.camera}};
    for (std::size_t i = 0; i < t.sweeps.size(); ++i) {
      const auto& sweep = t.sweeps[i];
      const toml::node& value = *sweep.values.get(at[i]);
      toml::table& table = sweep.table == "pattern" ? one.second.pattern
                           : sweep.table == "grain" ? one.second.grain
                                                    : one.second.camera;
      table.insert_or_assign(sweep.key, value);
      if (i > 0) one.first += ' ';
      one.first += sweep.table + "." + sweep.key + "=" + value_text(value);
    }
    out.push_back(std::move(one));
    std::size_t i = at.size();
    while (i > 0 && ++at[i - 1] == t.sweeps[i - 1].values.size()) at[--i] = 0;
    if (i == 0) return out;
  }
}

// `why`, saying which combination when there are several.
std::string labelled(const std::string& label, const std::string& why) {
  return label.empty() ? why : std::string(label).append(": ").append(why);
}

// Everything on disk a run needs. Every combination is read back by the real
// loaders here, before the first run.
Result<Stock> stock_up(const TrackTables& t, const fs::path& dir) {
  if (auto tray = put(dir / "tray_maps" / "track.txt", "circle,1.0\n\n\n1,0,0\n2,5,0\n"); !tray) {
    return fail(std::move(tray).error());
  }
  Stock stock;
  stock.trays = TrayLibrary::load(dir / "tray_maps");
  const TrayMap* tray = stock.trays.find(kTray);
  if (tray == nullptr) return fail(ErrorKind::Config, "the bench's tray did not load");
  const std::array<CalibrationPoint, 2> points{CalibrationPoint{"1", 10, 20}, CalibrationPoint{"2", 15, 20}};
  const CalibrationStore store(dir / "stage_calibrations");
  if (auto saved = store.save(*tray, kDevice, points); !saved) return fail(std::move(saved).error());

  auto expanded = expand(t);
  for (std::size_t i = 0; i < expanded.size(); ++i) {
    auto& [label, sections] = expanded[i];
    sections.pattern.insert_or_assign("kind", "dragonfly");
    sections.pattern.insert_or_assign("duration", t.duration_s);
    const std::string name = std::to_string(i);
    if (auto written = put(dir / "patterns" / (name + ".toml"), toml_text(sections.pattern)); !written) {
      return fail(std::move(written).error());
    }
    const auto cameras = CameraLibrary::parse(cameras_text(t, sections), "cameras.toml");
    const CameraConfig* camera = cameras.find(kDevice);
    if (!cameras.problems().empty() || camera == nullptr || !camera->sim_grain) {
      const std::string why = cameras.problems().empty() ? "its camera did not load" : cameras.problems().front();
      return fail(ErrorKind::Config, labelled(label, why));
    }
    Combination combination;
    combination.label = std::move(label);
    combination.pattern = name;
    combination.camera = *camera;
    combination.beam_radius_mm = camera->sim_grain->beam_radius_mm;
    combination.full_heat = t.output_percent / camera->sim_grain->reference_output_percent;
    stock.combinations.push_back(std::move(combination));
  }

  stock.patterns = PatternLibrary::load(dir / "patterns");
  for (const Combination& combination : stock.combinations) {
    if (stock.patterns.find(combination.pattern) != nullptr) continue;
    const auto problems = stock.patterns.problems();
    const auto it = std::find_if(problems.begin(), problems.end(), [&](const std::string& problem) {
      return problem.starts_with(combination.pattern + ":");
    });
    const std::string why = it != problems.end() ? *it : "its pattern did not load";
    return fail(ErrorKind::Config, labelled(combination.label, why));
  }
  return stock;
}

// Frames from the simulated camera, each written to `dir` when there is one.
class FrameTap final : public vision::IFrameSource {
 public:
  FrameTap(std::unique_ptr<SimTrayCamera> camera, fs::path dir) : camera_(std::move(camera)), dir_(std::move(dir)) {}

  Result<vision::Frame> grab() override {
    auto frame = camera_->grab();
    if (!frame || dir_.empty()) return frame;
    std::string name = std::to_string(next_++);
    name.insert(0, name.size() < 5 ? 5 - name.size() : 0, '0');
    const fs::path file = dir_ / (name + ".png");
    if (auto written = vision::write_png(file, frame->view()); !written) return fail(std::move(written).error());
    mark_as_user_file(file);
    return frame;
  }
  vision::FrameInfo info() const override { return camera_->info(); }

 private:
  std::unique_ptr<SimTrayCamera> camera_;
  fs::path dir_;
  int next_ = 0;
};

TransportOptions wire_options(const Clock& clock) {
  TransportOptions options;
  options.name = "laser_pc";
  options.clock = &clock;
  return options;
}

extraction::ChromiumOptions stage_options() {
  extraction::ChromiumOptions options;
  options.limits_mm = {{{-50, 50}, {-50, 50}, {-50, 50}}};
  options.signs = {1, 1, 1};
  return options;
}

// One run's stack. Declared in the order it must be built and, reversed, torn
// down, as tests/laser/laser_harness.hpp's LaserHarness.
struct Rig {
  ManualClock clock;
  extraction::ChromiumSim sim{clock};
  std::unique_ptr<SimTransport> wire = SimTransport::hooked(sim.hook(), wire_options(clock));
  extraction::ChromiumLaser driver{std::string(kDevice), *wire, stage_options()};
  CalibrationStore store;
  CorrectionStore corrections;
  LaserSystem system;
  SimTrayCamera* camera = nullptr;  // owned by the system
  std::optional<vision::DragonflyStep> step;  // the look of this poll, if there was one

  Rig(const Stock& stock, const fs::path& dir)
      : store(dir / "stage_calibrations"),
        corrections(dir / "stage_corrections"),
        system(std::string(kDevice), driver, stock.trays, store, &stock.patterns, &clock) {}

  // On the hole, the beam on.
  Result<void> stand_by(const TrackTables& t, const CameraConfig& config, std::uint64_t seed, const fs::path& frames) {
    if (auto opened = wire->open(); !opened) return opened;
    system.set_corrections(corrections);
    // Under the system's gate: it only records.
    system.set_step_observer([this](const vision::DragonflyStep& seen) { step = seen; });
    auto sees = std::make_unique<SimTrayCamera>(config, system.sight(), clock);
    camera = sees.get();
    camera->set_grain_seed(seed);
    if (auto attached = system.attach_camera(config, std::make_unique<FrameTap>(std::move(sees), frames), clock);
        !attached) {
      return attached;
    }
    if (auto tray = system.set_tray(kTray); !tray) return tray;
    if (auto moved = system.move_to_position("1", false); !moved) return moved;
    bool arrived = false;
    for (int i = 0; i < kStagePolls && !arrived; ++i) {
      auto busy = system.moving();
      if (!busy) return fail(std::move(busy).error());
      arrived = !*busy;
      if (!arrived) clock.advance(kStagePoll);
    }
    if (!arrived) return fail(ErrorKind::Timeout, "the stage did not reach the hole");
    if (auto on = system.enable(); !on) return on;
    if (auto set = system.extract(t.output_percent, extraction::ExtractUnits::Percent); !set) return set;
    return system.fire_laser();
  }
};

void put_fixed(std::string& out, double value, int decimals) {
  std::array<char, 340> buffer{};
  const auto end =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::fixed, decimals).ptr;
  out.append(buffer.data(), end);
}

std::string_view reason_text(vision::DragonflyStep::Reason reason) {
  using R = vision::DragonflyStep::Reason;
  switch (reason) {
    case R::Saturated: return "Saturated";
    case R::Deadband: return "Deadband";
    case R::Track: return "Track";
    case R::Search: return "Search";
    case R::PerimeterClamp: return "PerimeterClamp";
    case R::Miss: return "Miss";
    case R::Elapsed: return "Elapsed";
    case R::Invalid: return "Invalid";
  }
  return "";
}

constexpr std::string_view kTraceHeader =
    "combo,seed,t_s,beam_x,beam_y,grain_x,grain_y,heat,brightness,reason,saturation\n";

void trace_row(std::ostream& out, std::size_t combo, std::uint64_t seed, double t_s, const GrainView& view,
               const std::optional<vision::DragonflyStep>& step) {
  std::string row = std::to_string(combo) + "," + std::to_string(seed);
  for (const double value : {t_s, view.beam_mm.x, view.beam_mm.y, view.grain_mm.x, view.grain_mm.y, view.truth.heat,
                             view.truth.brightness}) {
    row += ',';
    put_fixed(row, value, 4);
  }
  row += ',';
  if (step) {
    row += reason_text(step->reason);
    row += ',';
    put_fixed(row, step->saturation, 4);
  } else {
    row += ',';
  }
  row += '\n';
  out << row;
}

// Polls the pattern to its end, scoring up to the end of its duration (what
// follows is the stage's trip home). When it ended, from its start, goes to
// `end_s`; a pattern error comes back as its text.
std::optional<std::string> follow(Rig& rig, const TrackTables& t, const Combination& combination, TrackScorer& scorer,
                                  std::size_t combo, std::uint64_t seed, std::ostream* trace, double& end_s) {
  auto& runner = *rig.system.pattern_runner();
  const Duration poll = std::chrono::milliseconds(t.poll_ms);
  const TimePoint started = rig.clock.now();
  if (auto begun = runner.execute_pattern(combination.pattern); !begun) return to_string(begun.error());
  while (true) {
    const double now_s = std::chrono::duration<double>(rig.clock.now() - started).count();
    auto busy = runner.running();
    const auto step = std::exchange(rig.step, std::nullopt);
    if (step) {
      scorer.look(step->reason);
      if (step->action == vision::DragonflyStep::Action::Move) scorer.moved();
    }
    end_s = now_s;
    if (!busy) return to_string(busy.error());
    if (!*busy) return std::nullopt;
    if (now_s < t.duration_s) {
      const auto view = rig.camera->observe();
      if (!view) return "the camera shows no grain";
      scorer.sample(now_s, std::hypot(view->grain_mm.x - view->beam_mm.x, view->grain_mm.y - view->beam_mm.y),
                    view->truth.heat);
      if (trace != nullptr) trace_row(*trace, combo, seed, now_s, *view, step);
    }
    if (now_s > t.duration_s + kDeadmanS) {
      (void)runner.stop_pattern();
      return "the pattern was still running 30 s after its duration";
    }
    rig.clock.advance(poll);
  }
}

struct Run {
  TrackScore score;
  std::optional<std::string> error;
};

Run run_one(const TrackTables& t, const Stock& stock, const fs::path& dir, std::size_t combo, std::uint64_t seed,
            std::ostream* trace, const fs::path& frames) {
  const Combination& combination = stock.combinations[combo];
  Rig rig(stock, dir);
  TrackScorer scorer(combination.beam_radius_mm, combination.full_heat, t.duration_s);
  Run out;
  double end_s = 0;
  if (auto ready = rig.stand_by(t, combination.camera, seed, frames); !ready) {
    out.error = to_string(ready.error());
  } else {
    out.error = follow(rig, t, combination, scorer, combo, seed, trace, end_s);
  }
  const auto stopped = rig.system.stop_laser();
  const auto disabled = rig.system.disable();
  if (!out.error && !stopped) out.error = to_string(stopped.error());
  if (!out.error && !disabled) out.error = to_string(disabled.error());
  out.score = scorer.finish(end_s);
  return out;
}

// Whether the trace can be written, leaving it as it was: --check writes nothing.
Result<void> check_trace(const fs::path& file) {
  std::error_code ec;
  const bool there = fs::exists(file, ec);
  {
    std::ofstream out(file, std::ios::binary | std::ios::app);
    if (!out) return fail(ErrorKind::Io, "cannot write " + file.string());
  }
  if (!there) fs::remove(file, ec);
  return {};
}

Result<void> check_frames(const fs::path& dir, bool keep) {
  std::error_code ec;
  const bool made = fs::create_directories(dir, ec);
  if (ec) return fail(ErrorKind::Io, "cannot make " + dir.string() + ": " + ec.message());
  const fs::path probe = dir / ".pychron_track_probe";
  auto written = put(probe, "");
  fs::remove(probe, ec);
  if (!written) return written;
  if (made && !keep) fs::remove(dir, ec);
  return {};
}

}  // namespace

Result<TrackScenario> parse_track_scenario(std::string_view toml, const std::string& file_name) {
  const toml::parse_result parsed = toml::parse(toml);
  if (!parsed) return fail(ErrorKind::Config, file_name + ": " + std::string(parsed.error().description()));
  auto tables = std::make_shared<TrackTables>();
  if (const std::string why = read_scenario(parsed.table(), *tables); !why.empty()) {
    return fail(ErrorKind::Config, file_name + ": " + why);
  }
  return TrackScenario{std::move(tables)};
}

Result<TrackScenario> load_track_scenario(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return fail(ErrorKind::Io, "cannot read " + file.string());
  std::ostringstream text;
  text << in.rdbuf();
  return parse_track_scenario(text.str(), file.filename().string());
}

Result<std::vector<TrackCombo>> run_track(const TrackScenario& scenario, const TrackOptions& options) {
  if (scenario.tables == nullptr) return fail(ErrorKind::Config, "no scenario");
  const TrackTables& t = *scenario.tables;

  const int seeds = options.seeds.value_or(t.seeds);
  if (seeds < 1 || seeds > kMaxRuns) return fail(ErrorKind::Config, "seeds: must be from 1 to 10000");
  std::int64_t runs = seeds;
  for (const auto& sweep : t.sweeps) {
    runs *= static_cast<std::int64_t>(sweep.values.size());
    if (runs > kMaxRuns) return fail(ErrorKind::Config, "more than 10000 runs: seeds times every sweep's values");
  }
  const std::uint64_t frames_seed = options.frames_seed.value_or(t.seed);
  if (!options.frames.empty() && (frames_seed < t.seed || frames_seed - t.seed >= static_cast<std::uint64_t>(seeds))) {
    return fail(ErrorKind::Config, "--frames-seed: " + std::to_string(frames_seed) + " is not one of the seeds run");
  }

  const Scratch scratch;
  if (const std::string why = scratch.problem(); !why.empty()) return fail(ErrorKind::Io, why);
  auto stock = stock_up(t, scratch.dir());
  if (!stock) return fail(std::move(stock).error());

  if (!options.trace.empty()) {
    if (auto writable = check_trace(options.trace); !writable) return fail(std::move(writable).error());
  }
  if (!options.frames.empty()) {
    if (auto writable = check_frames(options.frames, !options.check_only); !writable) {
      return fail(std::move(writable).error());
    }
  }
  if (options.check_only) return std::vector<TrackCombo>{};

  std::ofstream trace;
  if (!options.trace.empty()) {
    trace.open(options.trace, std::ios::binary | std::ios::trunc);
    trace << kTraceHeader;
    if (!trace) return fail(ErrorKind::Io, "cannot write " + options.trace.string());
  }

  std::vector<TrackCombo> combos;
  for (std::size_t c = 0; c < stock->combinations.size(); ++c) {
    TrackCombo combo;
    combo.label = stock->combinations[c].label;
    std::vector<TrackScore> scores;
    for (int i = 0; i < seeds; ++i) {
      const std::uint64_t seed = t.seed + static_cast<std::uint64_t>(i);
      const bool framed = c == 0 && seed == frames_seed;
      Run run = run_one(t, *stock, scratch.dir(), c, seed, options.trace.empty() ? nullptr : &trace,
                        framed ? options.frames : fs::path{});
      scores.push_back(run.score);
      if (run.error) {
        ++combo.failed;
        if (combo.first_error.empty()) combo.first_error = std::move(*run.error);
      }
    }
    combo.summary = summarize(scores);
    combos.push_back(std::move(combo));
  }
  if (!options.trace.empty()) {
    trace.close();
    if (!trace) return fail(ErrorKind::Io, "cannot write " + options.trace.string());
    mark_as_user_file(options.trace);
  }
  std::stable_sort(combos.begin(), combos.end(), [](const TrackCombo& a, const TrackCombo& b) {
    return a.summary.mean.on_pct > b.summary.mean.on_pct;
  });
  return combos;
}

std::string track_table(const std::vector<TrackCombo>& combos) {
  constexpr std::size_t kWidth = 9;
  constexpr std::size_t kName = 6;
  const auto cell = [](std::string& line, std::string_view text) {
    line.append(text.size() < kWidth ? kWidth - text.size() : 0, ' ');
    line += text;
  };
  const auto row = [&cell](std::string_view name, const TrackScore& s) {
    std::string line(name);
    line.append(kName - name.size(), ' ');
    for (const double value : {s.on_pct, s.rms_mm, s.max_mm, s.lock_s, s.lost, s.back_s, s.moves, s.heat_pct,
                               s.track_pct, s.hold_pct, s.sat_pct, s.search_pct}) {
      std::string text;
      put_fixed(text, value, 2);
      cell(line, text);
    }
    return line + '\n';
  };

  std::string out(kName, ' ');
  for (const std::string_view column :
       {"on%", "rms", "max", "lock_s", "lost", "back_s", "moves", "heat%", "track%", "hold%", "sat%", "search%"}) {
    cell(out, column);
  }
  out += '\n';
  for (const TrackCombo& combo : combos) {
    if (!combo.label.empty()) out += combo.label + '\n';
    out += row("mean", combo.summary.mean);
    out += row("worst", combo.summary.worst);
    if (combo.failed > 0) out += "failed " + std::to_string(combo.failed) + ": " + combo.first_error + '\n';
  }
  return out;
}

}  // namespace pychron::laser
