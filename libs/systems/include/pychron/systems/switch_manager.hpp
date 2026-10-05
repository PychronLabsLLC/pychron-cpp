#pragma once

// SwitchManager: owns every valve, manual valve and switch of one system and
// is the only path by which they are actuated.
//
// actuate(name, op, actor) runs, in order:
//   1. software lock   locked switches refuse every actor
//   2. owner           a claimed switch accepts only its owner
//   3. interlocks      opening a valve requires, per the manager's recorded
//                      states:
//                        - no negative interlock partner is open or Unknown.
//                          Negative interlocks are enforced both ways: if A
//                          lists B, A cannot open while B is not Closed and
//                          B cannot open while A is not Closed, so A and B
//                          are never open together.
//                        - every positive interlock is Open (checked when
//                          opening only; closing a prerequisite later is
//                          allowed).
//                      Closing is never interlocked.
//   4. command         IValveActuator::open/close at the switch's address
//   5. settle          wait settle time on the injected Clock
//   6. read back       IValveActuator::read must report the commanded state
// and publishes ValveChanged on success or ActuationFailed on any failure.
// Refusals in steps 1-3 are ErrorKind::Interlock and send nothing.
//
// Recorded state starts Unknown (which blocks as an interlock partner) until
// refresh() reads the hardware or an actuation reads it back. A failed
// command or read-back leaves the switch Unknown; a read-back that disagrees
// records what the hardware reported and fails with Protocol.
//
// Manual valves have no actuator: actuate() records the operator's report
// after the lock/owner checks, so they can take part in interlocks.
// Switches (pump power, heaters, ...) are actuated like valves but carry no
// interlocks of their own.
//
// Actuations are serialized: the interlock check, command, settle and
// read-back of one actuation are atomic with respect to every other, so two
// callers can never open mutually interlocked valves concurrently. Queries
// (state(), info(), ...) may be called from any thread at any time.
// Locks and ownership are in memory only.

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/config/system_config.hpp"
#include "pychron/core/error.hpp"
#include "pychron/core/events.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/devices/capabilities.hpp"
#include "pychron/devices/types.hpp"

namespace pychron::systems {

enum class SwitchOp { Open, Close };
enum class SwitchKind { Valve, ManualValve, Switch };

std::string_view to_string(SwitchOp op) noexcept;
std::string_view to_string(SwitchKind kind) noexcept;

struct SwitchSpec {
  std::string name;
  std::string description;
  SwitchKind kind = SwitchKind::Valve;
  std::string actuator;  // driver name; empty for manual valves
  ValveAddress address;
  std::vector<std::string> interlocks;           // Valve only
  std::vector<std::string> positive_interlocks;  // Valve only
  Duration settle{};
};

// What a switch has done since the manager was built. In memory only.
struct SwitchStats {
  int opens = 0;     // commands carried out (a manual valve: reports taken)
  int closes = 0;
  int failures = 0;  // commands sent that failed or read back wrong; a refusal sends nothing and is not one
  friend bool operator==(const SwitchStats&, const SwitchStats&) = default;
};

// Point-in-time copy of one switch, safe to hand across threads.
struct SwitchInfo {
  std::string name;
  std::string description;
  SwitchKind kind = SwitchKind::Valve;
  ValveState state = ValveState::Unknown;
  bool locked = false;
  std::string owner;  // empty if unclaimed
  SwitchStats stats;
};

// Resolves an actuator (driver) name to its IValveActuator, or nullptr. The
// actuators must outlive the manager.
using ActuatorLookup = std::function<IValveActuator*(const std::string& name)>;

struct SwitchManagerOptions {
  const Clock* clock = nullptr;  // settle waits and event stamps; SteadyClock if null
  SignalBus* bus = nullptr;      // events are dropped if null
};

class SwitchManager {
 public:
  using Options = SwitchManagerOptions;

  // Valves and manual valves from `config`. Config error (all problems
  // together) if an actuator cannot be resolved or a spec is invalid.
  static Result<std::unique_ptr<SwitchManager>> from_config(const config::SystemConfig& config,
                                                             const ActuatorLookup& lookup,
                                                             Options options = {});

  // Every spec is checked before any is added: unique non-empty names,
  // interlocks naming known switches, no self- or mixed interlock, only
  // valves carry interlocks, actuators resolve for valves and switches.
  static Result<std::unique_ptr<SwitchManager>> create(std::vector<SwitchSpec> specs,
                                                        const ActuatorLookup& lookup,
                                                        Options options = {});

  SwitchManager(const SwitchManager&) = delete;
  SwitchManager& operator=(const SwitchManager&) = delete;
  ~SwitchManager();

  // See the header comment for the full sequence. Unknown name is Config.
  Result<void> actuate(std::string_view name, SwitchOp op, std::string_view actor);

  // Puts a switch back in a state it held before (a line resuming where it
  // left off): actuate() without the software lock and ownership checks,
  // which guard against commands. Interlocks still apply.
  Result<void> restore(std::string_view name, SwitchOp op);

  // Reads every actuated switch back from hardware and records the result;
  // publishes ValveChanged for each state that changed. Read errors leave
  // that switch Unknown and are returned together (first kind wins).
  Result<void> refresh();

  // Software lock. Config error for an unknown name.
  Result<void> lock(std::string_view name);
  Result<void> unlock(std::string_view name);

  // Ownership. claim() succeeds if unowned or already owned by `actor`;
  // otherwise Interlock. release() by a non-owner is Interlock.
  Result<void> claim(std::string_view name, std::string_view actor);
  Result<void> release(std::string_view name, std::string_view actor);

  Result<ValveState> state(std::string_view name) const;
  Result<SwitchInfo> info(std::string_view name) const;
  std::vector<SwitchInfo> list() const;              // config order
  std::map<std::string, ValveState> states() const;  // for Snapshot
  bool contains(std::string_view name) const;

 private:
  struct Entry;
  SwitchManager(std::vector<std::unique_ptr<Entry>> entries, Options options);

  Entry* find(std::string_view name) const;
  Result<void> command(std::string_view name, SwitchOp op, const std::string_view* actor);
  Result<void> check_access(const Entry& e, std::string_view actor) const;
  Result<void> check_interlocks(const Entry& e) const;
  Result<void> drive(Entry& e, SwitchOp op);
  void settle(Duration d) const;
  bool record(Entry& e, ValveState s);  // true if the state changed
  void count(Entry& e, int SwitchStats::* what);
  Result<void> failed(const Entry& e, Error error);

  const Clock* clock_;
  SignalBus* bus_;
  std::vector<std::unique_ptr<Entry>> entries_;
  std::map<std::string, Entry*, std::less<>> by_name_;
  std::mutex actuation_;      // one actuation (or refresh) at a time
  mutable std::mutex state_;  // guards recorded state, locks and owners
};

}  // namespace pychron::systems
