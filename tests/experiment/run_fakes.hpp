#pragma once

// Fakes for run and executor tests: a spectrometer whose intensities grow
// linearly with clock time, valves, an extraction device, a script host whose
// scripts are C++ lambdas, and a plan library with one multicollect plan.

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "pychron/devices/extraction/interfaces.hpp"
#include "pychron/experiment/measurement/ports.hpp"
#include "pychron/experiment/persist/persister.hpp"
#include "pychron/experiment/plan/plan_library.hpp"
#include "pychron/experiment/run/run.hpp"
#include "pychron/scripting/script_host.hpp"

namespace pychron::experiment::fakes {

using namespace std::chrono_literals;

inline double secs(pychron::Duration d) { return std::chrono::duration<double>(d).count(); }

class FakeSpectrometer final : public measurement::ISpectrometerPort {
 public:
  explicit FakeSpectrometer(ManualClock& clock) : clock_(clock) {}

  Result<void> position(const plan::HopTarget& target) override {
    std::lock_guard lock(mutex_);
    at_ = target;
    ++moves;
    return {};
  }
  Result<void> protect(const std::string&, bool) override { return {}; }
  Result<void> start_acquisition(pychron::Duration integration) override {
    std::lock_guard lock(mutex_);
    if (acquiring) ++overlapping;  // two runs measuring at once
    integration_ = integration;
    acquiring = true;
    return {};
  }
  Result<std::optional<spectrometer::Reading>> next_reading(pychron::Duration) override {
    int n = 0;
    {
      std::lock_guard lock(mutex_);
      n = ++readings;
    }
    clock_.advance(integration_);
    spectrometer::Reading r;
    r.ts = clock_.now();
    r.integration = integration_;
    const double t = secs(clock_.now() - TimePoint{});
    const bool on_peak = at_ && !at_->mass;
    r.values["H1"] = spectrometer::Value{on_peak ? 1000 + t : 0.5, std::nullopt, std::nullopt, false};
    r.values["AX"] = spectrometer::Value{on_peak ? 100 + 0.1 * t : 0.5, std::nullopt, std::nullopt, false};
    if (on_reading) on_reading(n);
    return std::optional<spectrometer::Reading>(std::move(r));
  }
  void stop_acquisition() override { acquiring = false; }

  std::function<void(int)> on_reading;
  std::atomic<int> readings{0}, moves{0}, overlapping{0};
  std::atomic<bool> acquiring{false};

 private:
  ManualClock& clock_;
  std::mutex mutex_;
  std::optional<plan::HopTarget> at_;
  pychron::Duration integration_ = 1s;
};

class FakeValves final : public measurement::IValvePort {
 public:
  Result<void> open(const std::string& v) override { return set(v, true); }
  Result<void> close(const std::string& v) override { return set(v, false); }
  bool is_open(const std::string& v) {
    std::lock_guard lock(mutex_);
    return state[v];
  }
  std::vector<std::string> log;

 private:
  Result<void> set(const std::string& v, bool open) {
    std::lock_guard lock(mutex_);
    state[v] = open;
    log.push_back((open ? "open " : "close ") + v);
    return {};
  }
  std::mutex mutex_;
  std::map<std::string, bool> state;
};

// A stage that only records the trays it was given; "no-such-tray" is refused.
class FakeStage final : public extraction::IStage {
 public:
  Result<void> move_to_position(std::string_view, bool) override { return {}; }
  Result<void> set_axis(Axis, double) override { return {}; }
  Result<void> set_xy(double, double, double = 0) override { return {}; }
  Result<extraction::StagePosition> position() override { return extraction::StagePosition{}; }
  Result<bool> moving() override { return false; }
  Result<void> set_tray(std::string_view tray) override {
    if (tray == "no-such-tray") return fail(ErrorKind::Config, "no tray map 'no-such-tray'");
    std::lock_guard lock(mutex_);
    trays_.emplace_back(tray);
    return {};
  }
  std::vector<std::string> positions() const override { return {}; }
  std::vector<std::string> trays() const {
    std::lock_guard lock(mutex_);
    return trays_;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<std::string> trays_;
};

class FakeDevice final : public extraction::IExtractionDevice {
 public:
  FakeDevice() = default;
  explicit FakeDevice(std::string name, bool with_stage = false) : has_stage(with_stage), name_(std::move(name)) {}

  const std::string& device_name() const override { return name_; }
  extraction::IStage* stage() override { return has_stage ? &fake_stage : nullptr; }
  Result<void> enable() override {
    enabled = true;
    return {};
  }
  Result<void> disable() override {
    enabled = false;
    ++disables;
    return {};
  }
  Result<bool> is_enabled() override { return enabled.load(); }
  Result<void> extract(double value, extraction::ExtractUnits) override {
    output_ = value;
    ++extracts;
    return {};
  }
  Result<void> end_extract() override {
    output_ = 0;
    ++end_extracts;
    return {};
  }
  Result<double> output() override { return output_.load(); }
  bool supports(extraction::ExtractUnits) const override { return true; }

  std::atomic<bool> enabled{false};
  std::atomic<int> disables{0}, end_extracts{0}, extracts{0};
  bool has_stage = false;
  FakeStage fake_stage;

 private:
  std::string name_ = "fake_laser";
  std::atomic<double> output_{0};
};

// Scripts are named C++ functions; the source text is the function name.
class FakeScriptHost final : public scripting::IScriptHost {
 public:
  using Body = std::function<Result<void>(const scripting::ScriptEnvironment&, scripting::CancelToken&)>;

  bool available() const noexcept override { return true; }
  Result<scripting::CheckReport> check(const scripting::Script&, const scripting::ScriptEnvironment&) override {
    return scripting::CheckReport{};
  }
  Result<scripting::Estimate> estimate(const scripting::Script&, const scripting::ScriptEnvironment&) override {
    return scripting::Estimate{};
  }
  Result<scripting::ScriptResult> run(const scripting::Script& script, const scripting::ScriptEnvironment& env,
                                      scripting::CancelToken& token) override {
    Body body;
    {
      std::lock_guard lock(mutex_);
      ran.push_back(script.name);
      auto it = bodies.find(script.name);
      if (it != bodies.end()) body = it->second;
    }
    if (body)
      if (auto r = body(env, token); !r) return fail(r.error());
    scripting::ScriptResult out;
    out.sha = "sha-" + script.name;
    return out;
  }
  Result<scripting::ScriptResult> call_hook(const scripting::Script&, std::string_view, const scripting::ValueMap&,
                                            const scripting::ScriptEnvironment&, scripting::CancelToken&) override {
    return scripting::ScriptResult{};
  }

  std::vector<std::string> ran_scripts() {
    std::lock_guard lock(mutex_);
    return ran;
  }
  std::map<std::string, Body> bodies;

 private:
  std::mutex mutex_;
  std::vector<std::string> ran;
};

// Resolves any name to a script of that name.
class AnyScriptResolver final : public scripting::IScriptResolver {
 public:
  Result<scripting::Script> resolve(std::string_view name, scripting::ScriptKind from) const override {
    if (missing.contains(std::string(name))) return fail(ErrorKind::Config, "no script '" + std::string(name) + "'");
    scripting::Script s;
    s.name = std::string(name);
    s.text = "def main(): pass";
    s.kind = from;
    return s;
  }
  std::set<std::string> missing;
};

inline constexpr const char* kTestPlan = R"(
[plan]
name = "mc"
instrument_family = "fake"

[detectors]
reference = "H1"

[equilibration]
inlet = "B"
outlet = "C"
time_s = 5
inlet_delay_s = 1

[baseline]
after = true
counts = 3
mass = 34.2
settle_s = 1

[main]
cycles = 1
integration_s = 1

[[main.hops]]
positions = { Ar40 = "H1", Ar39 = "AX" }
counts = 10
settle_s = 1

[parameters]
expose = ["main.hops[0].counts"]
)";

inline plan::PlanLibrary test_plans() {
  plan::PlanLibrary lib;
  auto t = plan::parse_plan_template(kTestPlan, "mc.toml");
  if (t) lib.add(*t);
  return lib;
}

inline RunSpec unknown_run(const std::string& identifier, const std::string& plan = "mc") {
  RunSpec r;
  r.id.identifier = identifier;
  r.id.type = AnalysisType::Unknown;
  r.extraction.value = 5;
  r.extraction.script = "extract";
  r.post_equilibration = "post_eq";
  r.post_measurement = "post_meas";
  r.measurement.plan = plan;
  return r;
}

// A temporary lab: records, spool, pipeline, aliquots.
class Lab {
 public:
  Lab()
      : dir_(std::filesystem::temp_directory_path() /
             ("lab_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
              std::to_string(counter()++))),
        files(dir_ / "records"),
        spool(dir_ / "spool"),
        save(spool, files),
        aliquots(files) {}
  ~Lab() {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
  Lab(const Lab&) = delete;
  Lab& operator=(const Lab&) = delete;

  const std::filesystem::path& dir() const { return dir_; }

 private:
  static std::atomic<int>& counter() {
    static std::atomic<int> n{0};
    return n;
  }
  std::filesystem::path dir_;

 public:
  persist::FilePersister files;
  persist::Spool spool;
  persist::SavePipeline save;
  persist::AliquotAllocator aliquots;
};

}  // namespace pychron::experiment::fakes
