#pragma once

// Scriptable role fakes for Spectrometer / move protocol tests. Every call
// that matters for ordering is appended to a shared log.

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "pychron/devices/spectrometer/roles.hpp"

namespace pychron::spectrometer::testing {

using CallLog = std::vector<std::string>;

inline std::string fmt(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.3f", v);
  return buf;
}

struct FakePositioner : IMassPositioner {
  explicit FakePositioner(CallLog& call_log) : log(call_log) {}

  Axis native_axis() const override { return axis; }
  Result<void> set(double v) override {
    ++set_calls;
    if (fail_set_at && *fail_set_at == set_calls) {
      log.push_back("set-fail:" + fmt(v));
      return fail(ErrorKind::Io, "set failed", "magnet");
    }
    log.push_back("set:" + fmt(v));
    sets.push_back(v);
    value = v;
    moving_left = moving_polls;
    if (timeout_set_at && *timeout_set_at == set_calls) return fail(ErrorKind::Timeout, "no reply to set", "magnet");
    return {};
  }
  Result<double> read() override { return value; }
  Result<bool> moving() override {
    ++moving_calls;
    if (fail_moving) {
      log.emplace_back("moving-fail");
      return fail(ErrorKind::Io, "moving failed", "magnet");
    }
    if (moving_left > 0) {
      --moving_left;
      return true;
    }
    return false;
  }
  Limits limits() const override { return lim; }

  CallLog& log;
  Axis axis = Axis::Dac;
  Limits lim{0.0, 10.0};
  double value = 0.0;
  int moving_polls = 0;  // moving() reports true this many times after each set()
  int moving_left = 0;
  int moving_calls = 0;
  int set_calls = 0;
  std::optional<int> fail_set_at;     // 1-based set() call that fails with nothing written
  std::optional<int> timeout_set_at;  // 1-based set() call that is written, then reports Timeout
  bool fail_moving = false;
  std::vector<double> sets;
};

struct FakeControl : IDetectorControl {
  explicit FakeControl(CallLog& call_log) : log(call_log) {}

  Caps caps() const override { return caps_; }
  Result<void> protect(const ChannelId& ch, bool on) override {
    if ((on ? fail_protect : fail_unprotect).contains(ch)) {
      log.push_back(std::string(on ? "protect-fail:" : "unprotect-fail:") + ch);
      if (on) protected_[ch] = true;  // partially applied before the error
      return fail(ErrorKind::Io, "protect failed", ch);
    }
    log.push_back(std::string(on ? "protect:" : "unprotect:") + ch);
    protected_[ch] = on;
    return {};
  }
  Result<void> set_deflection(const ChannelId& ch, double v) override {
    if (!caps_.has(DetectorCap::Deflection)) return fail(unsupported(DetectorCap::Deflection));
    deflection[ch] = v;
    return {};
  }
  Result<double> read_deflection(const ChannelId& ch) override { return deflection[ch]; }
  Result<void> set_gain(const ChannelId& ch, double v) override {
    gain[ch] = v;
    return {};
  }
  Result<double> read_gain(const ChannelId& ch) override { return gain.contains(ch) ? gain[ch] : 1.0; }
  Result<void> set_cdd_voltage(const ChannelId& ch, double v) override {
    cdd[ch] = v;
    return {};
  }

  bool any_protected() const {
    for (const auto& [ch, on] : protected_) {
      if (on) return true;
    }
    return false;
  }

  CallLog& log;
  Caps caps_ = DetectorCap::Protect | DetectorCap::Deflection | DetectorCap::Gain | DetectorCap::CddVoltage;
  std::set<ChannelId> fail_protect, fail_unprotect;
  std::map<ChannelId, bool> protected_;
  std::map<ChannelId, double> deflection, gain, cdd;
};

struct FakeBlank : IBeamBlank {
  explicit FakeBlank(CallLog& call_log) : log(call_log) {}
  Result<void> blank(bool on) override {
    if (on ? fail_on : fail_off) {
      log.emplace_back(on ? "blank-fail" : "unblank-fail");
      return fail(ErrorKind::Io, "blank failed", "blank");
    }
    log.emplace_back(on ? "blank" : "unblank");
    blanked = on;
    return {};
  }
  CallLog& log;
  bool blanked = false;
  bool fail_on = false, fail_off = false;
};

struct FakeSource : IBeamSource {
  FakeSource() {
    specs.push_back(ParamSpec{SourceParam::HV, Unit::Volts, {0, 10000}, true, true, "HV"});
    specs.push_back(ParamSpec{SourceParam::TrapCurrent, Unit::MicroAmps, {0, 1000}, true, true, "Trap"});
  }
  Result<void> set_hv(double v) override {
    hv = v;
    return {};
  }
  Result<double> read_hv() override {
    ++hv_reads;
    if (fail_read_hv) return fail(ErrorKind::Timeout, "no reply to GetHighVoltage", "source");
    return hv;
  }
  std::span<const ParamSpec> params() const override { return specs; }
  Result<void> set_param(const ParamId& id, double v) override {
    if (std::get_if<SourceParam>(&id) && std::get<SourceParam>(id) == SourceParam::HV) {
      hv = v;
    } else {
      trap = v;
    }
    return {};
  }
  Result<Readback> read_param(const ParamId& id) override {
    if (std::get_if<SourceParam>(&id) && std::get<SourceParam>(id) == SourceParam::HV) return Readback{hv, hv};
    return Readback{trap, std::nullopt};
  }

  std::vector<ParamSpec> specs;
  double hv = 4500.0;
  double trap = 100.0;
  int hv_reads = 0;
  bool fail_read_hv = false;
};

struct FakeAcquirer : IIntensityAcquirer {
  explicit FakeAcquirer(std::vector<ChannelId> channel_ids) : chans(std::move(channel_ids)) {}
  std::vector<ChannelId> channels() const override { return chans; }
  bool integrates() const override { return true; }
  Result<void> configure(Duration d) override {
    if (active_next > 0) ++overlaps;
    ++configures;
    configured = d;
    if (on_configure) on_configure();
    return {};
  }
  Result<void> start() override {
    if (active_next > 0) ++overlaps;
    ++starts;
    note("start");
    if (fail_start) return fail(ErrorKind::Io, "start failed", "acquirer");
    return {};
  }
  Result<void> stop() override {
    ++stops;
    note("stop");
    if (on_stop) on_stop();
    return {};
  }
  Result<std::optional<Frame>> next(Duration) override {
    ++active_next;
    note("next-enter");
    if (on_next) on_next();  // may block, or call back into the engine
    note("next-exit");
    --active_next;
    std::lock_guard lock(m);
    if (frames.empty()) return std::optional<Frame>{};
    Frame f = frames.front();
    frames.pop_front();
    return std::optional<Frame>(f);
  }
  void push(Frame f) {
    std::lock_guard lock(m);
    frames.push_back(std::move(f));
  }

  void note(const char* what) {
    if (log == nullptr) return;
    std::lock_guard lock(m);
    log->emplace_back(what);
  }

  std::vector<ChannelId> chans;
  Duration configured{};
  int starts = 0, stops = 0;
  bool fail_start = false;
  std::atomic<int> configures{0};
  std::function<void()> on_configure;  // runs inside configure(); may block
  std::function<void()> on_next;   // runs inside next(), outside `m`
  std::function<void()> on_stop;   // runs inside stop(), after it is logged
  std::atomic<int> active_next{0};
  std::atomic<int> overlaps{0};    // configure()/start() seen while a next() was active
  CallLog* log = nullptr;          // optional order log: start, next-enter, next-exit, stop
  std::mutex m;
  std::deque<Frame> frames;
};

}  // namespace pychron::spectrometer::testing
