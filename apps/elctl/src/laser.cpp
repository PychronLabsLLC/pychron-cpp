#include "laser.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <locale>
#include <numbers>
#include <optional>
#include <sstream>
#include <thread>

#include "pychron/codecs/codec.hpp"
#include "pychron/devices/extraction/interfaces.hpp"
#include "pychron/experiment/lab/lab.hpp"
#include "pychron/laser/calibration_store.hpp"
#include "pychron/laser/laser_system.hpp"
#include "pychron/laser/pattern.hpp"
#include "pychron/laser/tray_camera.hpp"
#include "pychron/vision/finder.hpp"
#include "pychron/laser/tray_map.hpp"
#include "pychron/sim/sim_system.hpp"
#include "pychron/systems/extraction_line.hpp"

namespace elctl {

namespace {

namespace fs = std::filesystem;
using namespace pychron;
namespace lab = pychron::experiment::lab;

constexpr const char* kLaserUsage =
    "usage: elctl [-c <extraction_line.toml>] [--sim] laser <command> [--lab <dir>]\n"
    "  trays                                             tray maps and their calibrations\n"
    "  calibrate <device> <tray> point <hole> [--x X --y Y]\n"
    "                                                    the stage is on <hole>: record where it is\n"
    "                                                    (read from the device unless X and Y are given)\n"
    "  calibrate <device> <tray> center|right [--x X --y Y]\n"
    "                                                    the same, at the map's centre / east calibration hole\n"
    "  calibrate <device> <tray> show|clear\n"
    "  goto <device> <tray> <hole> [--timeout <s>]       move there and report the miss\n"
    "  autocenter <device> <tray> <hole> [--timeout <s>] move there and centre the hole with the camera;\n"
    "                                                    what is found is saved as the hole's correction\n"
    "  corrections <device> <tray> [clear [<hole>]]      where holes were found; or forget them\n"
    "  look <device> [--tray <tray>]                     what the camera's finder sees now; moves nothing\n"
    "  patterns                                          the lab's patterns: kind, points, length, time\n"
    "  pattern <device> <name> [--dry-run] [--timeout <s>]\n"
    "                                                    run a pattern about where the stage is (the laser is\n"
    "                                                    not fired); --dry-run prints its points instead\n";

// Locale-free fixed-point text.
std::string num(double value, int places = 3) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::fixed << std::setprecision(places) << value;
  std::string text = out.str();
  // "-0.000" reads as a sign error.
  if (text.starts_with('-') && text.find_first_not_of("-0.") == std::string::npos) text.erase(0, 1);
  return text;
}

std::string joined(const std::vector<std::string>& names) {
  std::string out;
  for (const auto& n : names) out += (out.empty() ? "" : ", ") + n;
  return out.empty() ? "none" : out;
}

std::string describe(const laser::Solution& s) {
  const auto& t = s.transform;
  return std::to_string(s.points) + (s.points == 1 ? " point" : " points") + ": centre " + num(t.cx) + ", " +
         num(t.cy) + "  rotation " + num(t.rotation * 180.0 / std::numbers::pi) + " deg  scale " + num(t.scale, 4) +
         "  rms " + num(s.rms_mm) + " mm";
}

struct Args {
  std::vector<std::string> words;  // everything that is not an option
  fs::path lab;
  std::optional<double> x, y, timeout;
  std::optional<std::string> tray;  // --tray, for look
  bool dry_run = false;
};

class Laser {
 public:
  Laser(Args args, const ExpGlobals& globals, Io io) : a_(std::move(args)), g_(globals), io_(io) {
    lab_ = lab::load_lab({a_.lab, g_.config, {}});
  }

  int trays() {
    for (const auto& p : lab_.trays.problems()) io_.err << "error: " << p << '\n';
    bool ok = lab_.trays.problems().empty();
    const auto names = lab_.trays.names();
    if (names.empty()) io_.out << "no tray maps in " << (a_.lab / "tray_maps").string() << '\n';
    for (const auto& name : names) {
      const laser::TrayMap& map = *lab_.trays.find(name);
      io_.out << name << "  " << map.holes().size() << " holes\n";
      for (const auto& device : lab_.extract_devices) {
        const auto status = lab_.calibrations->status(map, device);
        io_.out << "  " << device << ": ";
        switch (status.state) {
          case laser::CalibrationState::Ok:
            io_.out << "calibrated (" << status.solution->points << (status.solution->points == 1 ? " point" : " points")
                    << ", rms " << num(status.solution->rms_mm) << " mm)\n";
            for (const auto& c : cautions_of(map, device)) io_.out << "    check: " << c << '\n';
            break;
          case laser::CalibrationState::Missing:
            io_.out << "not calibrated\n";
            break;
          default:
            io_.out << to_string(status.state) << ": " << status.why << '\n';
            ok = false;
        }
      }
    }
    return ok ? kOk : kFailed;
  }

  // The device's camera, or why it has none.
  const laser::CameraConfig* camera() {
    if (const laser::CameraConfig* config = lab_.cameras.find(device_)) return config;
    const auto problems = lab_.cameras.problems_of(device_);
    if (problems.empty()) {
      failed(device_ + " has no camera: no [" + device_ + "] table in " + (a_.lab / "cameras.toml").string());
    } else {
      for (const auto& p : problems) failed("camera of " + device_ + ": " + p);
    }
    return nullptr;
  }

  // autocenter <device> <tray> <hole>
  int autocenter() {
    const std::string& hole = a_.words[3];
    if (map_->find(hole) == nullptr) return failed("no hole " + hole + " on tray " + map_->name());
    const laser::CameraConfig* config = camera();
    if (config == nullptr) return kFailed;
    // A recording never moves a stage: said before anything is opened.
    if (config->source == laser::CameraSource::Recorded) {
      return failed(laser::usable_for_autocenter(*config, true).error().what);
    }
    auto opened = open();
    if (!opened) return failed(opened.error().what);
    // And a simulated camera only over a simulated stage.
    const bool simulated = line_->sim() != nullptr && line_->sim()->chromium(device_) != nullptr;
    if (auto ok = laser::usable_for_autocenter(*config, simulated); !ok) return failed(ok.error().what);
    laser::LaserSystem system(device_, **opened, lab_.trays, *lab_.calibrations);
    system.set_corrections(*lab_.corrections);
    auto frames = laser::make_frame_source(*config, a_.lab, system.sight(), line_->clock());
    if (!frames) return failed(frames.error().what);
    if (auto ok = system.attach_camera(*config, std::move(*frames), line_->clock()); !ok) return failed(ok.error().what);
    if (auto r = system.set_tray(map_->name()); !r) return failed(r.error().what);
    if (auto r = system.move_to_position(hole, true); !r) return failed(r.error().what);
    io_.out << "moving to hole " << hole << " and centring it\n";
    io_.out.flush();

    const auto step = std::chrono::milliseconds(50);
    const auto limit = std::chrono::duration<double>(a_.timeout.value_or(120));
    const auto started = std::chrono::steady_clock::now();
    const int interrupts = interrupt_count().load();
    for (;;) {
      auto moving = system.moving();
      if (!moving) return failed(moving.error().what);  // on_failure = fail says it here
      if (!*moving) break;
      const bool interrupted = interrupt_count().load() != interrupts;
      if (interrupted || std::chrono::steady_clock::now() - started > limit) {
        return failed((interrupted ? "interrupted" : "not centred after " + num(limit.count(), 1) + " s") + "; " +
                      stopped(system));
      }
      std::this_thread::sleep_for(step);
    }
    const laser::AutocenterOutcome outcome = system.last_autocenter();
    auto at = system.position();
    const std::string where = at ? num(at->x) + ", " + num(at->y) : std::string("an unknown position");
    if (outcome.result != laser::AutocenterOutcome::Result::Converged) {
      // Whatever the lab's on_failure says a queue should do, here it is the answer.
      return failed("hole " + hole + " on " + map_->name() + ": autocenter failed (" +
                    std::string(to_string(outcome.reason)) + "); the stage is back at " + where);
    }
    io_.out << "hole " << hole << ": converged after " << outcome.iterations << (outcome.iterations == 1 ? " look" : " looks")
            << "; moved " << num(outcome.moved_mm.x) << ", " << num(outcome.moved_mm.y) << " mm; residual "
            << num(outcome.residual_mm) << " mm; now at " << where << '\n';
    if (!outcome.note.empty()) {
      io_.err << "warning: " << outcome.note << '\n';
      return kFailed;
    }
    return kOk;
  }

  // corrections <device> <tray> [clear [<hole>]]
  int corrections() {
    const auto& w = a_.words;
    const auto status = lab_.calibrations->status(*map_, device_);
    if (w.size() >= 4) {  // clear
      if (w.size() == 5) {
        if (status.state != laser::CalibrationState::Ok) return failed(status.why);
        if (auto r = lab_.corrections->clear_hole(*map_, device_, status.fingerprint, w[4]); !r) return failed(r.error().what);
        io_.out << "forgot the correction of hole " << w[4] << '\n';
        return kOk;
      }
      if (auto r = lab_.corrections->clear(device_, map_->name()); !r) return failed(r.error().what);
      io_.out << "forgot the corrections of tray " << map_->name() << " on " << device_ << '\n';
      return kOk;
    }
    if (status.state != laser::CalibrationState::Ok) return failed(status.why);
    const auto loaded = lab_.corrections->load(*map_, device_, status.fingerprint);
    if (!loaded) return failed(loaded.error().what);
    if (loaded->empty()) {
      io_.out << "no corrections for tray " << map_->name() << " on " << device_ << '\n';
      return kOk;
    }
    // In the tray's order, not the text order of the ids.
    for (const auto& hole : map_->holes()) {
      const auto it = loaded->find(hole.id);
      if (it == loaded->end()) continue;
      const auto nominal = status.solution->transform.to_stage(hole.x, hole.y);
      io_.out << "hole " << hole.id << "  " << num(it->second.x) << ", " << num(it->second.y) << "  "
              << num(std::hypot(it->second.x - nominal.x, it->second.y - nominal.y)) << " mm from calibrated  residual "
              << num(it->second.residual_mm) << " mm  " << it->second.found << '\n';
    }
    return kOk;
  }

  // look <device> [--tray <tray>]
  int look() {
    device_ = a_.words[1];
    if (int rc = check_device(); rc != kOk) return rc;
    const laser::CameraConfig* config = camera();
    if (config == nullptr) return kFailed;
    double radius_mm = 0.5;
    const laser::TrayMap* tray = nullptr;
    if (a_.tray) {
      tray = lab_.trays.find(*a_.tray);
      if (tray == nullptr) {
        return failed("no tray map '" + *a_.tray + "' in " + (a_.lab / "tray_maps").string() +
                      " (it has: " + joined(lab_.trays.names()) + ")");
      }
      radius_mm = tray->dimension() / 2;
    }

    // A recording needs no hardware; the simulated camera looks from where
    // the stage is, at the tray it is told.
    static const SteadyClock wall;
    std::unique_ptr<laser::LaserSystem> system;
    Result<std::unique_ptr<vision::IFrameSource>> frames = fail(ErrorKind::Config, "no camera");
    if (config->source == laser::CameraSource::Recorded) {
      frames = laser::make_frame_source(*config, a_.lab, {}, wall);
    } else {
      auto opened = open();
      if (!opened) return failed(opened.error().what);
      system = std::make_unique<laser::LaserSystem>(device_, **opened, lab_.trays, *lab_.calibrations);
      if (tray != nullptr) {
        if (auto r = system->set_tray(tray->name()); !r) return failed(r.error().what);
      }
      frames = laser::make_frame_source(*config, a_.lab, system->sight(), line_->clock());
    }
    if (!frames) return failed(frames.error().what);

    vision::SimpleFinder finder;
    vision::FinderParams params;
    params.mode = vision::FinderMode::Hole;
    params.expected_radius_px = radius_mm * config->px_per_mm;
    std::vector<double> xs, ys, radii;
    int width = 0, height = 0;
    for (int i = 0; i < config->frames_per_step; ++i) {
      auto frame = (*frames)->grab();
      if (!frame) return failed("the camera gave no frame: " + frame.error().what);
      width = frame->width;
      height = frame->height;
      const double aim_x = (frame->width - 1) / 2.0 + config->aim_offset_px.x;
      const double aim_y = (frame->height - 1) / 2.0 + config->aim_offset_px.y;
      // The hole nearest the aim point.
      const vision::Target* best = nullptr;
      const auto targets = finder.find(frame->view(), params);
      for (const auto& t : targets) {
        if (best == nullptr || std::hypot(t.center_px.x - aim_x, t.center_px.y - aim_y) <
                                   std::hypot(best->center_px.x - aim_x, best->center_px.y - aim_y)) {
          best = &t;
        }
      }
      if (best == nullptr) continue;
      xs.push_back(best->center_px.x - aim_x);
      ys.push_back(best->center_px.y - aim_y);
      radii.push_back(best->radius_px);
    }
    if (xs.size() * 2 <= static_cast<std::size_t>(config->frames_per_step)) {
      return failed("no hole of radius " + num(params.expected_radius_px, 1) + " px seen in " +
                    std::to_string(width) + " x " + std::to_string(height) + " frames (" + std::to_string(xs.size()) +
                    " of " + std::to_string(config->frames_per_step) + ")");
    }
    const auto median = [](std::vector<double> v) {
      std::sort(v.begin(), v.end());
      return v[v.size() / 2];
    };
    const double ox = median(xs), oy = median(ys);
    const auto move = config->map().to_mm({ox, oy});
    io_.out << "a hole: offset " << num(ox) << ", " << num(oy) << " px from the aim point; move " << num(move.x) << ", "
            << num(move.y) << " mm would centre it; radius " << num(median(radii)) << " px (" << xs.size() << " of "
            << config->frames_per_step << " frames)\n";
    return kOk;
  }

  int patterns() {
    for (const auto& p : lab_.patterns.problems()) io_.err << "error: " << p << '\n';
    const auto names = lab_.patterns.names();
    if (names.empty()) io_.out << "no patterns in " << (a_.lab / "patterns").string() << '\n';
    bool ok = lab_.patterns.problems().empty();
    for (const auto& name : names) {
      const laser::Pattern& pattern = *lab_.patterns.find(name);
      const auto path = laser::pattern_path(pattern, pattern.seed.value_or(0));
      if (!path) {
        io_.err << "error: " << path.error().what << '\n';
        ok = false;
        continue;
      }
      io_.out << name << "  " << to_string(pattern.kind) << "  " << summary(pattern, *path) << '\n';
    }
    return ok ? kOk : kFailed;
  }

  // pattern <device> <name>
  int pattern() {
    device_ = a_.words[1];
    if (int rc = check_device(); rc != kOk) return rc;
    const std::string& name = a_.words[2];
    const laser::Pattern* pattern = lab_.patterns.find(name);
    if (pattern == nullptr) {
      for (const auto& p : lab_.patterns.problems()) {
        if (p.starts_with(name + ": ")) return failed("pattern " + p);
      }
      return failed("no pattern '" + name + "' in " + (a_.lab / "patterns").string() +
                    " (it has: " + joined(lab_.patterns.names()) + ")");
    }
    if (a_.dry_run) {
      const auto path = laser::pattern_path(*pattern, pattern->seed.value_or(0));
      if (!path) return failed(path.error().what);
      for (std::size_t i = 0; i < path->size(); ++i) {
        io_.out << std::setw(3) << i + 1 << "  " << std::setw(7) << num((*path)[i].x) << "," << std::setw(7)
                << num((*path)[i].y) << '\n';
      }
      io_.out << summary(*pattern, *path) << " at " << num(pattern->velocity) << " mm/s\n";
      if (pattern->kind == laser::PatternKind::Random && !pattern->seed) {
        io_.out << "(a random walk with no seed: each run's points differ)\n";
      }
      return kOk;
    }

    auto opened = open();
    if (!opened) return failed(opened.error().what);
    laser::LaserSystem system(device_, **opened, lab_.trays, *lab_.calibrations, &lab_.patterns);
    auto* runner = system.pattern_runner();
    if (runner == nullptr) return failed(device_ + " has no stage to run a pattern on");
    if (auto r = runner->execute_pattern(name); !r) return failed(r.error().what);
    const auto path = laser::pattern_path(*pattern, pattern->seed.value_or(0));
    io_.out << "pattern " << name << ": " << (path ? summary(*pattern, *path) : std::string("running")) << '\n';
    io_.out.flush();

    const auto step = std::chrono::milliseconds(50);
    // Time for the pattern itself, and as much again for the arrival checks.
    const double expected = path ? laser::path_length(*path) / pattern->velocity : 60;
    const auto limit = std::chrono::duration<double>(a_.timeout.value_or(2 * expected + 60));
    const auto started = std::chrono::steady_clock::now();
    const int interrupts = interrupt_count().load();
    for (;;) {
      auto running = runner->running();
      if (!running) return failed(running.error().what);
      if (!*running) break;
      const bool interrupted = interrupt_count().load() != interrupts;
      if (interrupted || std::chrono::steady_clock::now() - started > limit) {
        const std::string why = interrupted ? "interrupted" : "still running after " + num(limit.count(), 1) + " s";
        if (auto r = runner->stop_pattern(); !r) {
          return failed(why + "; the pattern could not be stopped (" + r.error().what + ")");
        }
        auto at = system.position();
        return failed(why + "; the stage was stopped" + (at ? " at " + num(at->x) + ", " + num(at->y) : std::string{}));
      }
      std::this_thread::sleep_for(step);
    }
    auto at = system.position();
    if (!at) return failed(at.error().what);
    io_.out << "ended at " << num(at->x) << ", " << num(at->y) << '\n';
    return kOk;
  }

  int check_device() {
    if (std::find(lab_.extract_devices.begin(), lab_.extract_devices.end(), device_) == lab_.extract_devices.end()) {
      return failed("no extraction device '" + device_ + "' in " + g_.config.string() +
                    " (it has: " + joined(lab_.extract_devices) + ")");
    }
    return kOk;
  }

  // device and tray are words 1 and 2 of both calibrate and goto.
  int with_tray(int (Laser::*then)()) {
    device_ = a_.words[1];
    if (int rc = check_device(); rc != kOk) return rc;
    map_ = lab_.trays.find(a_.words[2]);
    if (map_ == nullptr) {
      for (const auto& p : lab_.trays.problems()) io_.err << "error: " << p << '\n';
      return failed("no tray map '" + a_.words[2] + "' in " + (a_.lab / "tray_maps").string() +
                    " (it has: " + joined(lab_.trays.names()) + ")");
    }
    return (this->*then)();
  }

  int show() {
    const auto loaded = lab_.calibrations->load(device_, map_->name());
    if (!loaded) return failed(loaded.error().what);
    if (loaded->has_value()) {
      for (const auto& p : (*loaded)->points) {
        io_.out << "hole " << p.hole << "  stage " << num(p.x) << ", " << num(p.y) << '\n';
      }
    }
    const auto status = lab_.calibrations->status(*map_, device_);
    if (status.state != laser::CalibrationState::Ok) {
      io_.out << status.why << '\n';
      return kFailed;
    }
    io_.out << describe(*status.solution) << '\n';
    for (const auto& c : cautions_of(*map_, device_)) io_.err << "warning: " << c << '\n';
    return kOk;
  }

  int clear() {
    if (auto r = lab_.calibrations->clear(device_, map_->name()); !r) return failed(r.error().what);
    io_.out << "cleared the calibration of tray " << map_->name() << " on " << device_ << '\n';
    return kOk;
  }

  int point(std::string hole) {
    if (map_->find(hole) == nullptr) return failed("no hole " + hole + " on tray " + map_->name());
    double x = 0, y = 0;
    if (a_.x) {
      x = *a_.x;
      y = *a_.y;
    } else {
      auto opened = open();
      if (!opened) return failed(opened.error().what);
      auto* stage = (*opened)->stage();
      if (stage == nullptr) return failed(device_ + " has no stage");
      auto at = stage->position();
      if (!at) return failed(at.error().what);
      x = at->x;
      y = at->y;
    }
    io_.out << "hole " << hole << ": stage at " << num(x) << ", " << num(y) << '\n';

    std::vector<laser::CalibrationPoint> points;
    const auto loaded = lab_.calibrations->load(device_, map_->name());
    if (!loaded) return failed(loaded.error().what);
    if (loaded->has_value()) {
      if ((*loaded)->tray_sha256 == map_->sha256()) {
        points = (*loaded)->points;
      } else {
        io_.err << "warning: the tray map has changed since " << device_ << " was calibrated for it; "
                << "the earlier points are dropped and the calibration starts again\n";
      }
    }
    const auto same = std::find_if(points.begin(), points.end(), [&](const auto& p) { return p.hole == hole; });
    if (same != points.end()) *same = {hole, x, y};
    else points.push_back({hole, x, y});

    // save() solves first and refuses a set that does not solve.
    if (auto r = lab_.calibrations->save(*map_, device_, points); !r) return failed(r.error().what);
    const auto status = lab_.calibrations->status(*map_, device_);
    if (!status.solution) return failed(status.why);
    io_.out << describe(*status.solution) << '\n';
    for (const auto& c : cautions_of(*map_, device_)) io_.err << "warning: " << c << '\n';
    if (status.solution->rms_mm > map_->dimension()) {
      io_.err << "warning: the points miss by more than a hole (" << num(map_->dimension())
              << " mm); check which holes they were taken on\n";
    }
    return kOk;
  }

  int calibrate() {
    const auto& w = a_.words;  // calibrate <device> <tray> <verb> [hole]
    const std::string& verb = w[3];
    const bool takes_xy = verb == "point" || verb == "center" || verb == "right";
    if (!takes_xy && (a_.x || a_.y)) return usage("--x and --y go with point, center and right");
    if (verb == "point") {
      if (w.size() != 5) return usage("calibrate ... point needs a hole");
      return point(w[4]);
    }
    if (w.size() != 4) return usage("unexpected '" + w[4] + "'");
    if (verb == "show") return show();
    if (verb == "clear") return clear();
    if (verb == "center" || verb == "right") {
      const auto hole = verb == "center" ? map_->center_hole() : map_->right_hole();
      if (!hole) {
        return failed("tray map " + map_->name() + " names no calibration holes; use: calibrate " + device_ + " " +
                      map_->name() + " point <hole>");
      }
      return point(*hole);
    }
    return usage("unknown calibrate command '" + verb + "'");
  }

  int go_to() {
    const std::string& hole = a_.words[3];
    auto opened = open();
    if (!opened) return failed(opened.error().what);
    laser::LaserSystem system(device_, **opened, lab_.trays, *lab_.calibrations);
    if (auto r = system.set_tray(map_->name()); !r) return failed(r.error().what);
    const auto status = system.calibration();
    const laser::Hole* h = map_->find(hole);
    if (h == nullptr) return failed("no hole " + hole + " on tray " + map_->name());
    if (auto r = system.move_to_position(hole, false); !r) return failed(r.error().what);
    const auto target = status.solution->transform.to_stage(h->x, h->y);  // the move was accepted: it is calibrated
    io_.out << "moving to hole " << hole << " at " << num(target.x) << ", " << num(target.y) << '\n';
    io_.out.flush();

    const auto step = std::chrono::milliseconds(100);
    const auto limit = std::chrono::duration<double>(a_.timeout.value_or(60));
    const auto started = std::chrono::steady_clock::now();
    const int interrupts = interrupt_count().load();
    for (;;) {
      auto moving = system.moving();
      if (!moving) return failed(moving.error().what);
      if (!*moving) break;
      const bool interrupted = interrupt_count().load() != interrupts;
      if (interrupted || std::chrono::steady_clock::now() - started > limit) {
        return failed((interrupted ? "interrupted" : "still moving after " + num(limit.count(), 1) + " s") + "; " +
                      stopped(system));
      }
      std::this_thread::sleep_for(step);
    }
    auto at = system.position();
    if (!at) return failed(at.error().what);
    io_.out << "hole " << hole << ": stage at " << num(at->x) << ", " << num(at->y) << "  miss "
            << num(std::hypot(at->x - target.x, at->y - target.y)) << " mm\n";
    return kOk;
  }

  int usage(const std::string& message) {
    io_.err << "elctl laser: " << message << '\n' << kLaserUsage;
    return kUsage;
  }

  bool has_line() {
    if (lab_.line) return true;
    io_.err << "error: no extraction line config at " << g_.config.string() << '\n';
    for (const auto& p : lab_.problems) io_.err << "error: " << p << '\n';
    return false;
  }

 private:
  int failed(const std::string& what) {
    io_.err << "error: " << what << '\n';
    return kFailed;
  }

  // "8 points  8.000 mm  8.0 s": the whole path, and how long it takes at the
  // pattern's speed (the pauses at each point come on top).
  static std::string summary(const laser::Pattern& pattern, const std::vector<laser::StageXY>& path) {
    const double length = laser::path_length(path);
    return std::to_string(path.size()) + " points  " + num(length) + " mm  " + num(length / pattern.velocity, 1) + " s";
  }

  // Stops the stage and says where; says so when it could not be stopped.
  std::string stopped(extraction::IStage& stage) {
    if (auto r = stage.stop(); !r) return "the stage could not be stopped (" + r.error().what + ") and may still be moving";
    auto at = stage.position();
    if (!at) return "the stage was stopped";
    return "the stage was stopped at " + num(at->x) + ", " + num(at->y);
  }

  // What the stored calibration cannot rule out (laser::cautions).
  std::vector<std::string> cautions_of(const laser::TrayMap& map, const std::string& device) {
    const auto loaded = lab_.calibrations->load(device, map.name());
    if (!loaded || !loaded->has_value()) return {};
    const auto solved = laser::solve(map, (*loaded)->points);
    if (!solved) return {};
    return laser::cautions(map, (*loaded)->points, *solved);
  }

  // The line, started, and the device on it. The line is kept for as long
  // as this command lives.
  Result<extraction::IExtractionDevice*> open() {
    systems::ExtractionLine::Options options;
    options.force_sim = g_.sim;
    auto line = systems::ExtractionLine::load(g_.config, std::nullopt, options);
    if (!line) return fail(line.error());
    line_ = std::move(*line);
    if (auto r = line_->start(); !r) return fail(r.error());
    auto* device = dynamic_cast<extraction::IExtractionDevice*>(line_->device(device_));
    if (device == nullptr) return fail(ErrorKind::Config, device_ + " is not an extraction device");
    // Is it what the config says it is? (A Chromium answers Sys.ID?.)
    if (auto r = device->prepare(); !r) return fail(r.error());
    return device;
  }

  Args a_;
  const ExpGlobals& g_;
  Io io_;
  lab::Lab lab_;
  std::string device_;
  const laser::TrayMap* map_ = nullptr;
  std::unique_ptr<systems::ExtractionLine> line_;
};

}  // namespace

int laser_command(const std::vector<std::string>& args, const ExpGlobals& globals, Io io) {
  const auto usage = [&](const std::string& message) {
    if (!message.empty()) io.err << "elctl laser: " << message << '\n';
    io.err << kLaserUsage;
    return static_cast<int>(kUsage);
  };
  Args a;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& x = args[i];
    if (x == "--lab" || x == "--x" || x == "--y" || x == "--timeout") {
      if (i + 1 >= args.size()) return usage(x + " needs a value");
      const std::string& v = args[++i];
      if (x == "--lab") {
        a.lab = v;
        continue;
      }
      const auto number = codec::parse_decimal(v);
      if (!number) return usage(x + " needs a number, not '" + v + "'");
      if (x == "--x") a.x = *number;
      else if (x == "--y") a.y = *number;
      else if (*number <= 0) return usage("--timeout must be above 0");
      else a.timeout = *number;
    } else if (x == "--tray") {
      if (i + 1 >= args.size()) return usage("--tray needs a value");
      a.tray = args[++i];
    } else if (x == "--dry-run") {
      a.dry_run = true;
    } else if (x.starts_with("--")) {
      return usage("unexpected '" + x + "'");
    } else {
      a.words.push_back(x);
    }
  }
  if (a.words.empty()) return usage("");
  if (a.x.has_value() != a.y.has_value()) return usage("--x and --y go together");
  if (a.lab.empty()) a.lab = !globals.lab.empty() ? globals.lab : globals.config.parent_path();
  if (a.lab.empty()) a.lab = ".";

  const std::string verb = a.words[0];
  if (a.dry_run && verb != "pattern") return usage("--dry-run goes with pattern");
  if (a.tray && verb != "look") return usage("--tray goes with look");
  if (verb == "autocenter") {
    if (a.words.size() != 4) return usage("autocenter needs <device> <tray> <hole>");
    if (a.x) return usage("--x and --y go with calibrate");
    Laser laser(std::move(a), globals, io);
    return laser.has_line() ? laser.with_tray(&Laser::autocenter) : kFailed;
  }
  if (verb == "corrections") {
    const bool clear = a.words.size() >= 4 && a.words[3] == "clear";
    if (a.words.size() < 3 || a.words.size() > 5 || (a.words.size() >= 4 && !clear)) {
      return usage("corrections needs <device> <tray>, and optionally: clear [<hole>]");
    }
    if (a.x || a.timeout) return usage("corrections takes no --x, --y or --timeout");
    Laser laser(std::move(a), globals, io);
    return laser.has_line() ? laser.with_tray(&Laser::corrections) : kFailed;
  }
  if (verb == "look") {
    if (a.words.size() != 2) return usage("look needs <device>");
    if (a.x || a.timeout) return usage("look takes no --x, --y or --timeout");
    Laser laser(std::move(a), globals, io);
    return laser.has_line() ? laser.look() : kFailed;
  }
  if (verb == "patterns") {
    if (a.words.size() != 1) return usage("patterns takes no arguments");
    if (a.x || a.timeout) return usage("patterns takes no --x, --y or --timeout");
    Laser laser(std::move(a), globals, io);
    return laser.patterns();  // a folder of files: no line config needed
  }
  if (verb == "pattern") {
    if (a.words.size() != 3) return usage("pattern needs <device> <name>");
    if (a.x) return usage("--x and --y go with calibrate");
    Laser laser(std::move(a), globals, io);
    return laser.has_line() ? laser.pattern() : kFailed;
  }
  if (verb == "trays") {
    if (a.words.size() != 1) return usage("trays takes no arguments");
    if (a.x || a.timeout) return usage("trays takes no --x, --y or --timeout");
    Laser laser(std::move(a), globals, io);
    return laser.has_line() ? laser.trays() : kFailed;
  }
  if (verb == "calibrate") {
    if (a.words.size() < 4) return usage("calibrate needs <device> <tray> and point, center, right, show or clear");
    if (a.timeout) return usage("--timeout goes with goto");
    Laser laser(std::move(a), globals, io);
    return laser.has_line() ? laser.with_tray(&Laser::calibrate) : kFailed;
  }
  if (verb == "goto") {
    if (a.words.size() != 4) return usage("goto needs <device> <tray> <hole>");
    if (a.x) return usage("--x and --y go with calibrate");
    Laser laser(std::move(a), globals, io);
    return laser.has_line() ? laser.with_tray(&Laser::go_to) : kFailed;
  }
  return usage("unknown command '" + verb + "'");
}

}  // namespace elctl
