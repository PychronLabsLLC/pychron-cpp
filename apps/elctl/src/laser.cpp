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
#include "pychron/laser/tray_map.hpp"
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
    "  goto <device> <tray> <hole> [--timeout <s>]       move there and report the miss\n";

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

  // device and tray are words 1 and 2 of both calibrate and goto.
  int with_tray(int (Laser::*then)()) {
    device_ = a_.words[1];
    if (std::find(lab_.extract_devices.begin(), lab_.extract_devices.end(), device_) == lab_.extract_devices.end()) {
      return failed("no extraction device '" + device_ + "' in " + g_.config.string() +
                    " (it has: " + joined(lab_.extract_devices) + ")");
    }
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
      // There is no stage stop in the device interface: the stage goes on to
      // its target, and this only stops waiting for it.
      if (interrupt_count().load() != interrupts) return failed("interrupted; the stage may still be moving");
      if (std::chrono::steady_clock::now() - started > limit) {
        return failed("still moving after " + num(limit.count(), 1) + " s; the stage may still be moving");
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
