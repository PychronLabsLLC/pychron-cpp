#pragma once

// ChromiumSim: the laser PC a `chromium` driver talks to, for sim runs and
// tests. It answers as the vendor's command reference says a Chromium does
// (docs/superpowers/specs/2026-10-04-chromium-protocol-survey.md, section 2a):
// a query gets one CR-terminated line, an action gets nothing, and anything
// refused gets "?<n>".
//
// Time comes from the injected Clock: a stage move travels each axis at its
// commanded speed (microns per second) and stops exactly on target. State is
// advanced lazily to clock.now() on every command and accessor.
//
// Hook contract: one SimTransport per simulator; hook() captures `this`, so
// the simulator must outlive the transport. Thread-safe.

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/codecs/chromium.hpp"
#include "pychron/core/clock.hpp"
#include "pychron/transport/bytes.hpp"
#include "pychron/transport/sim_transport.hpp"

namespace pychron::extraction {

class ChromiumSim {
 public:
  using Microns = codec::chromium::Microns;

  explicit ChromiumSim(const Clock& clock);

  // The reply to what was written: "" for an action that succeeds.
  SimTransport::Hook hook();

  // Scenario controls.
  void trip_interlock(std::string name);  // Laser.Status? is non-zero; Laser.Fire answers ?4
  void clear_interlocks();
  void add_scan(Microns start);           // scans are numbered from 1
  void put_on_limit(char axis, int side); // 'x' | 'y' | 'z'; side -1, 0 (off), +1
  void set_id(std::string id);            // what Sys.ID? answers
  // The next command starting with `command_prefix` (case-insensitive) is
  // refused with ?<code>, once.
  void fail_next(std::string command_prefix, int code);

  // State.
  bool enabled() const;
  bool firing() const;
  double output() const;
  Microns position() const;
  std::vector<std::string> log() const;  // every command received, without its terminator

 private:
  std::string handle(std::string_view command);
  std::string laser(std::string_view command, std::string_view args);
  std::string stage(std::string_view command, std::string_view args);
  std::string scans(std::string_view command, std::string_view args);
  void advance();  // to clock_.now(); callers hold mutex_
  void start_move(const std::array<double, 3>& target, const std::array<double, 3>& speed);
  bool at_rest() const;

  const Clock& clock_;
  mutable std::mutex mutex_;
  std::string id_ = "CHROMIUM 2013.12.30.0";
  bool enabled_ = false;
  bool firing_ = false;
  double output_ = 0;
  std::vector<std::string> interlocks_;
  std::array<double, 3> at_{};      // microns
  std::array<double, 3> target_{};
  std::array<double, 3> speed_{};   // microns per second; 0: that axis is not moving
  TimePoint advanced_{};
  std::array<int, 3> limits_{};
  std::vector<Microns> scans_;
  struct Refusal {
    std::string prefix;
    int code = 0;
  };
  std::vector<Refusal> refusals_;
  std::vector<std::string> log_;
};

}  // namespace pychron::extraction
