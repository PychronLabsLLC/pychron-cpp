#pragma once

// Recording fakes for the script host tests: every call appends to a shared
// log so tests can assert the command sequence.

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "pychron/devices/extraction/services.hpp"
#include "pychron/scripting/script_host.hpp"

namespace pychron::scripting::testing {

struct CallLog {
  std::mutex mutex;
  std::vector<std::string> calls;
  void add(std::string c) {
    std::lock_guard lock(mutex);
    calls.push_back(std::move(c));
  }
  std::vector<std::string> snapshot() {
    std::lock_guard lock(mutex);
    return calls;
  }
  bool contains(const std::string& c) {
    auto s = snapshot();
    return std::find(s.begin(), s.end(), c) != s.end();
  }
  std::size_t count(const std::string& c) {
    auto s = snapshot();
    return static_cast<std::size_t>(std::count(s.begin(), s.end(), c));
  }
  void clear() {
    std::lock_guard lock(mutex);
    calls.clear();
  }
};

inline std::string num(double v) {
  auto s = std::to_string(v);
  s.erase(s.find_last_not_of('0') + 1);
  if (s.back() == '.') s.pop_back();
  return s;
}

class FakeValves final : public extraction::IValveService {
 public:
  explicit FakeValves(CallLog& log) : log_(log) {}
  std::set<std::string, std::less<>> known{"A", "B", "C"};
  std::set<std::string, std::less<>> open_set;
  std::set<std::string, std::less<>> interlocked;

  Result<void> open(std::string_view n) override {
    if (auto r = ok(n); !r) return r;
    if (interlocked.contains(n)) return fail(ErrorKind::Interlock, "interlocked", std::string(n));
    log_.add("open " + std::string(n));
    open_set.insert(std::string(n));
    return {};
  }
  Result<void> close(std::string_view n) override {
    if (auto r = ok(n); !r) return r;
    log_.add("close " + std::string(n));
    open_set.erase(std::string(n));
    return {};
  }
  Result<void> lock(std::string_view n) override {
    log_.add("lock " + std::string(n));
    return ok(n);
  }
  Result<void> unlock(std::string_view n) override {
    log_.add("unlock " + std::string(n));
    return ok(n);
  }
  Result<bool> is_open(std::string_view n) override {
    if (auto r = ok(n); !r) return fail(r.error());
    return open_set.contains(n);
  }
  Result<bool> is_closed(std::string_view n) override {
    auto o = is_open(n);
    if (!o) return o;
    return !*o;
  }
  bool contains(std::string_view n) const override { return known.contains(n); }
  std::vector<std::string> names() const override { return {known.begin(), known.end()}; }

 private:
  Result<void> ok(std::string_view n) {
    if (!known.contains(n)) return fail(ErrorKind::Config, "unknown valve " + std::string(n));
    return {};
  }
  CallLog& log_;
};

class FakePressure final : public extraction::IPressureService {
 public:
  Result<double> get_pressure(std::string_view, std::string_view) override { return 1e-8; }
  Result<double> get_manometer_pressure(std::string_view) override { return 12.5; }
};

// Laser with a stage and pattern runner.
class FakeLaser final : public extraction::IExtractionDevice,
                        public extraction::ILaserDevice,
                        public extraction::IStage,
                        public extraction::IPatternRunner {
 public:
  explicit FakeLaser(CallLog& log) : log_(log) {}

  const std::string& device_name() const override { return name_; }
  Result<void> prepare() override { return rec("prepare"); }
  Result<void> enable() override { enabled_ = true; return rec("enable"); }
  Result<void> disable() override { enabled_ = false; return rec("disable"); }
  Result<bool> is_enabled() override { return enabled_; }
  Result<void> extract(double v, extraction::ExtractUnits u) override {
    output_ = v;
    return rec("extract " + num(v) + " " + std::string(extraction::to_string(u)));
  }
  Result<void> end_extract() override { output_ = 0; return rec("end_extract"); }
  Result<double> output() override { return output_; }
  bool supports(extraction::ExtractUnits) const override { return true; }
  ILaserDevice* laser() override { return this; }
  IStage* stage() override { return this; }
  IPatternRunner* pattern_runner() override { return this; }

  Result<void> fire_laser() override { return rec("fire_laser"); }
  Result<void> stop_laser() override { return rec("stop_laser"); }
  Result<bool> is_firing() override { return false; }
  Result<void> warmup() override { return rec("warmup"); }

  Result<void> move_to_position(std::string_view p, bool) override {
    return rec("move_to_position " + std::string(p));
  }
  Result<void> set_axis(Axis a, double v) override {
    return rec(std::string("set_axis ") + (a == Axis::X ? "x" : a == Axis::Y ? "y" : "z") + " " +
               num(v));
  }
  Result<void> set_xy(double x, double y, double = 0) override { return rec("set_xy " + num(x) + " " + num(y)); }
  Result<extraction::StagePosition> position() override { return extraction::StagePosition{}; }
  // Never arrives while stage_stuck (for cancel tests), until stopped.
  Result<bool> moving() override { return stage_stuck && !stage_stopped; }
  Result<void> stop() override {
    if (!stage_can_stop) return fail(extraction::not_supported("stage stop", "fake"));
    stage_stopped = true;
    return rec("stop");
  }
  Result<void> set_tray(std::string_view t) override { return rec("set_tray " + std::string(t)); }
  std::vector<std::string> positions() const override { return {"1", "2"}; }

  Result<void> execute_pattern(std::string_view p) override {
    pattern_running = true;
    return rec("execute_pattern " + std::string(p));
  }
  // Runs until stopped (for cancel tests) unless pattern_finishes.
  Result<bool> running() override { return pattern_running && !pattern_finishes; }
  Result<void> stop_pattern() override {
    pattern_running = false;
    return rec("stop_pattern");
  }
  std::vector<std::string> patterns() const override { return {"spiral"}; }

  std::atomic<bool> pattern_running{false};
  bool pattern_finishes = true;
  std::atomic<bool> stage_stuck{false};
  std::atomic<bool> stage_stopped{false};
  bool stage_can_stop = true;

 private:
  Result<void> rec(std::string c) {
    log_.add(std::move(c));
    return {};
  }
  CallLog& log_;
  std::string name_ = "co2";
  bool enabled_ = false;
  double output_ = 0;
};

class FakeResources final : public IResourceService {
 public:
  std::map<std::string, double, std::less<>> values;
  std::set<std::string, std::less<>> held;
  Result<bool> try_acquire(std::string_view n) override {
    return held.insert(std::string(n)).second;
  }
  Result<void> release(std::string_view n) override {
    held.erase(std::string(n));
    return {};
  }
  Result<void> set_value(std::string_view n, double v) override {
    values[std::string(n)] = v;
    return {};
  }
  Result<double> get_value(std::string_view n) override {
    auto it = values.find(n);
    return it == values.end() ? 0.0 : it->second;
  }
};

class FakeIntensity final : public IIntensitySource {
 public:
  Result<double> intensity(std::string_view key) override {
    if (key == "Ar40") return 250.0;
    return fail(ErrorKind::Config, "unknown key " + std::string(key));
  }
};

class FakeMeasurementApi final : public IMeasurementApi {
 public:
  explicit FakeMeasurementApi(CallLog& log) : log_(log) {}
  Result<void> position(std::string_view i, std::string_view d) override {
    return rec("position " + std::string(i) + " " + std::string(d));
  }
  Result<void> acquire(int counts, double t) override {
    return rec("acquire " + std::to_string(counts) + " " + num(t));
  }
  Result<void> open(std::string_view v) override { return rec("api.open " + std::string(v)); }
  Result<void> close(std::string_view v) override { return rec("api.close " + std::string(v)); }
  Result<void> add_conditional(std::string_view s) override {
    return rec("add_conditional " + std::string(s));
  }
  Result<void> truncate(bool quick) override { return rec(quick ? "truncate quick" : "truncate"); }
  void log(std::string_view m) override { log_.add("log " + std::string(m)); }

 private:
  Result<void> rec(std::string c) {
    log_.add(std::move(c));
    return {};
  }
  CallLog& log_;
};

// A laser line with everything wired and example scripts resolvable.
struct Rig {
  CallLog log;
  FakeValves valves{log};
  FakePressure pressure;
  FakeLaser laser{log};
  FakeResources resources;
  FakeIntensity intensity;
  FakeMeasurementApi api{log};
  std::unique_ptr<IScriptResolver> resolver;
  ScriptEnvironment env;

  Rig() {
    resolver = std::make_unique<DirectoryScriptResolver>(PYCHRON_SCRIPTING_TEST_SCRIPTS_DIR);
    env.line = extraction::ExtractionServices{&laser, &valves, &pressure};
    env.resources = &resources;
    env.intensity = &intensity;
    env.measurement = &api;
    env.resolver = resolver.get();
    env.context = make_context({{"duration", 0.02}, {"cleanup", 0.01}, {"position", std::string("2")},
                                {"extract_value", 5.0}, {"run_identifier", std::string("12345-01A")}},
                               {{"settle", 0.01}, {"flag", true}});
  }
};

}  // namespace pychron::scripting::testing
