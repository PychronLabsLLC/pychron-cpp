#pragma once

// A simulated Isotopx NGX controller for SimTransport::hooked(hook, options,
// events), so tests run the real NgxLink, NgxSpectrometer and NgxValves
// against an in-memory model (NGX driver spec section 7). Replies follow the
// legacy simulator and the codec: E00 for success, Exx for errors, bare
// values for GETMASS, "setpoint,readback" for GSO, OPEN / CLOSED for valves.
// StartAcq N emits N "#EVENT:ACQ" lines one second apart on the model's
// clock and then "#EVENT:ACQ.B"; values go on the wire in reverse channel
// order, as the instrument sends them.

#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/transport/sim_transport.hpp"

namespace pychron::spectrometer {

struct NgxSimModel {
  std::mutex mutex;  // the hook and the event source lock it; tests may too

  const Clock* clock = nullptr;  // event timing; a SteadyClock when null
  std::string reply_terminator = "\r\n";

  // Session.
  std::string banner = "Isotopx NGX ready";
  bool banner_pending = true;  // sent by the first poll after (re)open
  std::string login_reply = "E00";
  bool logged_in = false;
  std::string user, password;  // credentials seen in the last Login

  // Magnet and source.
  double mass = 0.0;
  std::map<std::string, double> params;  // by mnemonic ("IE", "YF", ...)

  // Acquisition: values in channel order.
  std::vector<double> values;
  bool emit_acq_b = true;
  std::string start_acq_reply = "E00";
  struct Run {
    int seconds = 1;
    std::string rcs_id;
    TimePoint start{};
    int emitted = 0;  // ACQ lines sent
    bool done = false;
  };
  std::optional<Run> run;
  int runs_started = 0;

  // Valves.
  std::map<std::string, bool> valves;  // by address; true = open
  bool sab = false;
  int unbracketed_actuations = 0;  // Open/CloseValve without SAB 1
  int status_e00 = 0;              // the next GetValveStatus replies answer E00 this often

  // Faults.
  int hold_replies = 0;               // the next replies are held back until release_held()
  std::deque<std::string> held;       // held replies; release_held() makes them due
  bool release_due = false;
  std::string event_before_next_reply;  // an event line sent just before the next reply

  // Every command received, without terminator, in order.
  std::vector<std::string> commands;

  // Due events and released replies, for the event source (caller holds mutex).
  std::string due_locked();
  void release_held() {
    std::lock_guard lock(mutex);
    release_due = true;
  }
};

SimTransport::Hook ngx_sim_hook(std::shared_ptr<NgxSimModel> model);
SimTransport::Unsolicited ngx_sim_events(std::shared_ptr<NgxSimModel> model);

}  // namespace pychron::spectrometer
