#include "pychron/laser/camera.hpp"

#include <cmath>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>

#include <toml++/toml.hpp>

namespace pychron::laser {

namespace {

// Reads one table; the first thing wrong ends it.
class Reader {
 public:
  Reader(const toml::table& table, std::string path) : table_(table), path_(std::move(path)) {}

  bool ok() const { return why_.empty(); }
  const std::string& why() const { return why_; }

  void number(const char* key, double& out, double low, double high, bool above_low = false) {
    const toml::node* node = take(key);
    if (node == nullptr) return;
    const auto value = as_number(*node);
    if (!value) return fail(key, "expected a number");
    if (*value < low || *value > high || (above_low && *value <= low)) {
      return fail(key, std::string("must be ") + (above_low ? "above " : "from ") + text(low) + " to " + text(high));
    }
    out = *value;
  }
  void whole(const char* key, int& out, int low, int high) {
    const toml::node* node = take(key);
    if (node == nullptr) return;
    const auto* i = node->as_integer();
    if (i == nullptr) return fail(key, "expected a whole number");
    if (i->get() < low || i->get() > high) {
      return fail(key, "must be from " + std::to_string(low) + " to " + std::to_string(high));
    }
    out = static_cast<int>(i->get());
  }
  void flag(const char* key, bool& out) {
    const toml::node* node = take(key);
    if (node == nullptr) return;
    const auto* b = node->as_boolean();
    if (b == nullptr) return fail(key, "expected true or false");
    out = b->get();
  }
  void pair(const char* key, StageXY& out) {
    const toml::node* node = take(key);
    if (node == nullptr) return;
    const auto* a = node->as_array();
    if (a == nullptr || a->size() != 2) return fail(key, "expected [x, y]");
    const auto x = as_number(*a->get(0));
    const auto y = as_number(*a->get(1));
    if (!x || !y) return fail(key, "expected two finite numbers");
    out = {*x, *y};
  }
  std::optional<std::string> word(const char* key) {
    const toml::node* node = take(key);
    if (node == nullptr) return std::nullopt;
    auto value = node->value<std::string>();
    if (!value) fail(key, "expected text");
    return value;
  }
  const toml::table* sub(const char* key) {
    const toml::node* node = take(key);
    if (node == nullptr) return nullptr;
    const auto* t = node->as_table();
    if (t == nullptr) fail(key, "expected a table");
    return t;
  }
  void fail(const std::string& key, const std::string& what) {
    if (why_.empty()) why_ = path_ + "." + key + ": " + what;
  }
  // Every key not asked for is unknown.
  void finish() {
    for (const auto& [k, node] : table_) {
      const std::string key(k.str());
      if (!seen_.contains(key)) fail(key, "unknown key");
    }
  }
  void adopt(const Reader& inner) {
    if (why_.empty()) why_ = inner.why_;
  }

 private:
  const toml::node* take(const char* key) {
    seen_.insert(key);
    return ok() ? table_.get(key) : nullptr;
  }
  static std::optional<double> as_number(const toml::node& node) {
    double value = 0;
    if (const auto* f = node.as_floating_point()) value = f->get();
    else if (const auto* i = node.as_integer()) value = static_cast<double>(i->get());
    else return std::nullopt;
    if (!std::isfinite(value)) return std::nullopt;
    return value;
  }
  static std::string text(double value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << value;
    return out.str();
  }

  const toml::table& table_;
  std::string path_;
  std::string why_;
  std::set<std::string, std::less<>> seen_;
};

// Empty when `table` is a camera; else what is wrong with it.
std::string read_camera(const toml::table& table, CameraConfig& c) {
  Reader r(table, c.device);
  if (const auto source = r.word("source")) {
    if (*source == "sim") c.source = CameraSource::Sim;
    else if (*source == "recorded") c.source = CameraSource::Recorded;
    else r.fail("source", "expected sim or recorded");
  }
  r.number("px_per_mm", c.px_per_mm, 0, 10000, true);
  r.flag("flip_x", c.flip_x);
  r.flag("flip_y", c.flip_y);
  r.pair("aim_offset_px", c.aim_offset_px);
  int settle_ms = 200;
  r.whole("settle_ms", settle_ms, 0, 10000);
  c.settle = std::chrono::milliseconds(settle_ms);
  if (const auto frames = r.word("frames")) c.frames = *frames;
  if (r.ok() && c.source == CameraSource::Recorded && c.frames.empty()) {
    r.fail("frames", "a recorded source needs the directory of its frames");
  }
  if (const toml::table* sim = r.sub("sim")) {
    Reader s(*sim, c.device + ".sim");
    s.pair("tray_error_mm", c.sim_tray_error_mm);
    s.number("noise", c.sim_noise, 0, 1);
    s.whole("width", c.sim_width, 16, 4096);
    s.whole("height", c.sim_height, 16, 4096);
    s.pair("grain_offset_mm", c.sim_grain_offset_mm);
    s.pair("glow_drift_mm_per_s", c.sim_glow_drift_mm_per_s);
    s.number("glow_sigma_mm", c.sim_glow_sigma_mm, 0, 100, true);
    s.finish();
    r.adopt(s);
  }
  if (const toml::table* autocenter = r.sub("autocenter")) {
    Reader a(*autocenter, c.device + ".autocenter");
    a.number("tolerance_mm", c.tolerance_mm, 0, 10, true);
    a.number("max_step_mm", c.max_step_mm, 0, 10, true);
    a.whole("max_iterations", c.max_iterations, 1, 50);
    a.whole("frames_per_step", c.frames_per_step, 1, 15);
    if (const auto on_failure = a.word("on_failure")) {
      if (*on_failure == "continue") c.on_failure = OnAutocenterFailure::Continue;
      else if (*on_failure == "fail") c.on_failure = OnAutocenterFailure::Fail;
      else a.fail("on_failure", "expected continue or fail");
    }
    a.finish();
    r.adopt(a);
  }
  r.finish();
  return r.why();
}

}  // namespace

Result<void> usable_for_autocenter(const CameraConfig& config, bool stage_is_simulated) {
  if (config.source == CameraSource::Recorded) {
    return fail(ErrorKind::Config,
                "the camera of " + config.device + " is recorded frames, which do not follow the stage: they are for " +
                    "looking (elctl laser look), not for centring a hole",
                config.device);
  }
  if (config.source == CameraSource::Sim && !stage_is_simulated) {
    return fail(ErrorKind::Config,
                "the camera of " + config.device + " is simulated (source = \"sim\" in cameras.toml) and " +
                    config.device + " is a real laser: it would be centred on a tray that is not there. Remove the [" +
                    config.device + "] table until the laser has a live camera",
                config.device);
  }
  return {};
}

vision::CameraStageMap CameraConfig::map() const { return vision::CameraStageMap::from_scale(px_per_mm, flip_x, flip_y); }

CameraLibrary CameraLibrary::parse(std::string_view text, std::string file_name) {
  CameraLibrary lib;
  const toml::parse_result parsed = toml::parse(text);
  if (!parsed) {
    lib.unreadable_ = true;
    lib.problems_.push_back(file_name + ": " + std::string(parsed.error().description()));
    return lib;
  }
  for (const auto& [k, node] : parsed.table()) {
    const std::string device(k.str());
    std::string why;
    CameraConfig config;
    config.device = device;
    if (const auto* table = node.as_table()) why = read_camera(*table, config);
    else why = device + ": expected a table, [" + device + "]";
    if (why.empty()) {
      lib.cameras_.insert_or_assign(device, std::move(config));
    } else {
      lib.problems_.push_back(file_name + ": " + why);
      lib.by_device_[device].push_back(lib.problems_.back());
    }
  }
  return lib;
}

CameraLibrary CameraLibrary::load(const std::filesystem::path& file) {
  std::error_code ec;
  if (!std::filesystem::exists(file, ec)) return {};
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    CameraLibrary lib;
    lib.unreadable_ = true;
    lib.problems_.push_back(file.filename().string() + ": cannot be read");
    return lib;
  }
  std::ostringstream text;
  text << in.rdbuf();
  return parse(text.str(), file.filename().string());
}

const CameraConfig* CameraLibrary::find(std::string_view device) const {
  const auto it = cameras_.find(device);
  return it == cameras_.end() ? nullptr : &it->second;
}

std::vector<std::string> CameraLibrary::devices() const {
  std::vector<std::string> out;
  for (const auto& [name, camera] : cameras_) out.push_back(name);
  return out;
}

std::vector<std::string> CameraLibrary::problems_of(std::string_view device) const {
  if (unreadable_) return problems_;
  const auto it = by_device_.find(device);
  return it == by_device_.end() ? std::vector<std::string>{} : it->second;
}

}  // namespace pychron::laser
