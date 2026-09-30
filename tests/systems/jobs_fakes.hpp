#pragma once

// Rig for tuning-job tests: the sim-integrated config over the role fakes of
// spectrometer_fakes.hpp, plus an integrating acquirer whose signal is a
// function of the fakes' current setpoints. drive() plays the engine's
// Scheduler job and the passing of time while a job runs on another thread.

#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>

#include "pychron/systems/spectrometer/data_dir.hpp"
#include "pychron/systems/spectrometer/spectrometer.hpp"
#include "spectrometer_fakes.hpp"

namespace pychron::spectrometer::testing {

struct BeamAcquirer : IIntensityAcquirer {
  using Signal = std::function<double(const ChannelId&)>;

  BeamAcquirer(std::vector<ChannelId> chans, const Clock& clock) : chans(std::move(chans)), clock(clock) {}

  std::vector<ChannelId> channels() const override { return chans; }
  bool integrates() const override { return true; }
  Result<void> configure(Duration d) override {
    std::lock_guard lock(m);
    configured.push_back(d);
    span = d;
    return {};
  }
  Result<void> start() override { return {}; }
  Result<void> stop() override { return {}; }
  // Runs on the acquiring thread, right after the sweep set its axis.
  Result<void> trigger() override {
    std::lock_guard lock(m);
    Frame f;
    f.ts = clock.now();
    f.seq = ++seq;
    f.integrated = true;
    f.span = span;
    for (const auto& c : chans) f.values.emplace_back(c, signal ? signal(c) : 0.0);
    frames.push_back(std::move(f));
    return {};
  }
  Result<std::optional<Frame>> next(Duration) override {
    std::lock_guard lock(m);
    if (frames.empty()) return std::optional<Frame>{};
    Frame f = std::move(frames.front());
    frames.erase(frames.begin());
    return std::optional<Frame>(std::move(f));
  }

  std::vector<ChannelId> chans;
  const Clock& clock;
  Signal signal;
  std::mutex m;
  std::vector<Frame> frames;
  std::vector<Duration> configured;
  Duration span = std::chrono::seconds(1);
  std::uint64_t seq = 0;
};

struct JobRig {
  CallLog log;
  ManualClock clock{TimePoint{} + std::chrono::seconds(100)};
  SignalBus bus;
  Scheduler scheduler{clock, &bus, Scheduler::Options{0}};
  FakePositioner positioner{log};
  FakeControl control{log};
  FakeBlank blank{log};
  FakeSource source;
  BeamAcquirer acquirer{{"H2", "H1", "AX", "L1", "L2", "CDD"}, clock};
  std::mutex sleeps_mutex;
  std::vector<Duration> sleeps;  // Sweep settle waits
  std::unique_ptr<Spectrometer> spec;

  JobRig() {
    const auto path = std::filesystem::path(PYCHRON_EXAMPLE_CONFIGS_DIR) / "spectrometer.sim-integrated.toml";
    auto d = cfg::load_spectrometer(path);
    EXPECT_TRUE(d.has_value()) << (d ? "" : d.error().what);
    if (!d) return;
    std::map<std::string, FieldTable> tables;
    for (const auto& [name, tf] : d->tables) tables.emplace(name, to_field_table(tf));
    SpectrometerRoles roles;
    roles.positioner = &positioner;
    roles.source = &source;
    roles.acquirers = {{"sim", &acquirer}};
    roles.detector_control = &control;
    roles.beam_blank = &blank;
    Spectrometer::Options options;
    options.sleep = [this](Duration dt) { clock.advance(dt); };
    auto made = Spectrometer::create(d->config, MolecularWeights(d->weights), std::move(tables), std::move(roles),
                                     SpectrometerContext{clock, scheduler, bus}, options);
    EXPECT_TRUE(made.has_value()) << (made ? "" : made.error().what);
    if (made) spec = std::move(*made);
  }

  // Sleep hook for Sweep::Options: records and advances the clock.
  std::function<void(Duration)> sweep_sleep() {
    return [this](Duration dt) {
      {
        std::lock_guard lock(sleeps_mutex);
        sleeps.push_back(dt);
      }
      clock.advance(dt);
    };
  }

  // Advances time and polls the engine until `fut` is ready.
  template <class T>
  T drive(std::future<T>& fut) {
    using namespace std::chrono_literals;
    while (fut.wait_for(1ms) != std::future_status::ready) {
      clock.advance(10ms);
      spec->acquisition().poll(0);
    }
    return fut.get();
  }
};

}  // namespace pychron::spectrometer::testing
