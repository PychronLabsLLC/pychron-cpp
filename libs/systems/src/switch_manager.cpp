#include "pychron/systems/switch_manager.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <set>
#include <utility>

#include "pychron/devices/types.hpp"

namespace pychron::systems {

std::string_view to_string(SwitchOp op) noexcept { return op == SwitchOp::Open ? "open" : "close"; }

std::string_view to_string(SwitchKind kind) noexcept {
  switch (kind) {
    case SwitchKind::Valve:
      return "valve";
    case SwitchKind::ManualValve:
      return "manual_valve";
    case SwitchKind::Switch:
      return "switch";
  }
  return "unknown";
}

struct SwitchManager::Entry {
  SwitchSpec spec;
  IValveActuator* actuator = nullptr;  // null for manual valves
  IValveActuator* reader = nullptr;    // state_source's device, when there is one
  std::vector<Entry*> exclusive;       // negative interlocks, both directions
  std::vector<Entry*> positive;
  // Guarded by SwitchManager::state_.
  ValveState state = ValveState::Unknown;
  bool locked = false;
  std::string owner;
  SwitchStats stats;

  SwitchInfo info() const { return {spec.name, spec.description, spec.kind, state, locked, owner, stats}; }
};

namespace {

const Clock& default_clock() {
  static const SteadyClock clock;
  return clock;
}

ValveState target_of(SwitchOp op) { return op == SwitchOp::Open ? ValveState::Open : ValveState::Closed; }

ValveState inverse(ValveState s) {
  switch (s) {
    case ValveState::Open:
      return ValveState::Closed;
    case ValveState::Closed:
      return ValveState::Open;
    default:
      return s;
  }
}

template <class C>
std::optional<StateSource> state_source_of(const C& c) {
  if (!c.state_source) return std::nullopt;
  return StateSource{c.state_source->driver, ValveAddress{c.state_source->address}, c.state_source->inverted};
}

std::string join(const std::vector<std::string>& lines) {
  std::string out;
  for (const auto& l : lines) {
    if (!out.empty()) out += '\n';
    out += l;
  }
  return out;
}

}  // namespace

Result<std::unique_ptr<SwitchManager>> SwitchManager::from_config(const config::SystemConfig& config,
                                                                   const ActuatorLookup& lookup,
                                                                   Options options) {
  std::vector<SwitchSpec> specs;
  specs.reserve(config.valves.size() + config.manual_valves.size() + config.switches.size());
  for (const auto& v : config.valves) {
    SwitchSpec s;
    s.name = v.name;
    s.description = v.description;
    s.kind = SwitchKind::Valve;
    s.actuator = v.actuator;
    s.address = ValveAddress{v.address};
    s.interlocks = v.interlocks;
    s.positive_interlocks = v.positive_interlocks;
    s.settle = std::chrono::milliseconds(v.settle_ms);
    s.inverted = v.inverted;
    s.state_source = state_source_of(v);
    s.verify = v.verify;
    specs.push_back(std::move(s));
  }
  for (const auto& m : config.manual_valves) {
    SwitchSpec s;
    s.name = m.name;
    s.description = m.description;
    s.kind = SwitchKind::ManualValve;
    specs.push_back(std::move(s));
  }
  for (const auto& w : config.switches) {
    SwitchSpec s;
    s.name = w.name;
    s.description = w.description;
    s.kind = SwitchKind::Switch;
    s.actuator = w.actuator;
    s.address = ValveAddress{w.address};
    s.settle = std::chrono::milliseconds(w.settle_ms);
    s.inverted = w.inverted;
    s.state_source = state_source_of(w);
    s.verify = w.verify;
    specs.push_back(std::move(s));
  }
  return create(std::move(specs), lookup, options);
}

Result<std::unique_ptr<SwitchManager>> SwitchManager::create(std::vector<SwitchSpec> specs,
                                                              const ActuatorLookup& lookup, Options options) {
  std::vector<std::string> problems;
  std::vector<std::unique_ptr<Entry>> entries;
  std::map<std::string, Entry*, std::less<>> by_name;

  for (auto& spec : specs) {
    auto e = std::make_unique<Entry>();
    e->spec = std::move(spec);
    const auto& s = e->spec;
    if (s.name.empty()) {
      problems.push_back("switch with empty name");
      continue;
    }
    if (by_name.contains(s.name)) {
      problems.push_back("duplicate switch name '" + s.name + "'");
      continue;
    }
    const std::string what = std::string(to_string(s.kind)) + " '" + s.name + "'";
    if (s.kind != SwitchKind::ManualValve) {
      e->actuator = lookup ? lookup(s.actuator) : nullptr;
      if (!e->actuator) problems.push_back(what + ": actuator '" + s.actuator + "' is not a valve actuator");
      if (s.state_source) {
        e->reader = lookup ? lookup(s.state_source->driver) : nullptr;
        if (!e->reader) {
          problems.push_back(what + ": state_source '" + s.state_source->driver + "' is not a valve actuator");
        }
        if (!s.verify) problems.push_back(what + ": verify = false reads nothing back; it cannot have a state_source");
      }
    } else if (s.inverted || s.state_source || !s.verify) {
      problems.push_back(what + ": a manual valve has no actuator to invert, read or verify");
    }
    if (s.kind != SwitchKind::Valve && (!s.interlocks.empty() || !s.positive_interlocks.empty())) {
      problems.push_back(std::string(to_string(s.kind)) + " '" + s.name + "': only valves carry interlocks");
    }
    by_name.emplace(s.name, e.get());
    entries.push_back(std::move(e));
  }

  auto resolve = [&](const Entry& e, const std::string& other) -> Entry* {
    if (other == e.spec.name) {
      problems.push_back("valve '" + e.spec.name + "' cannot interlock with itself");
      return nullptr;
    }
    auto it = by_name.find(other);
    if (it == by_name.end()) {
      problems.push_back("valve '" + e.spec.name + "': unknown switch '" + other + "'");
      return nullptr;
    }
    return it->second;
  };
  for (auto& e : entries) {
    for (const auto& other : e->spec.interlocks) {
      Entry* o = resolve(*e, other);
      if (!o) continue;
      e->exclusive.push_back(o);
      o->exclusive.push_back(e.get());
    }
    for (const auto& other : e->spec.positive_interlocks) {
      const auto& neg = e->spec.interlocks;
      if (std::find(neg.begin(), neg.end(), other) != neg.end()) {
        problems.push_back("valve '" + e->spec.name + "': '" + other +
                           "' is both an interlock and a positive interlock");
        continue;
      }
      if (Entry* o = resolve(*e, other)) e->positive.push_back(o);
    }
  }
  if (!problems.empty()) return fail(ErrorKind::Config, join(problems));

  for (auto& e : entries) {
    std::sort(e->exclusive.begin(), e->exclusive.end());
    e->exclusive.erase(std::unique(e->exclusive.begin(), e->exclusive.end()), e->exclusive.end());
  }
  return std::unique_ptr<SwitchManager>(new SwitchManager(std::move(entries), options));
}

SwitchManager::SwitchManager(std::vector<std::unique_ptr<Entry>> entries, Options options)
    : clock_(options.clock ? options.clock : &default_clock()),
      bus_(options.bus),
      // The clock's calendar time: under a simulated clock, simulated time.
      wall_(options.wall ? std::move(options.wall) : std::function<WallTime()>([clock = clock_] {
        return std::chrono::floor<std::chrono::seconds>(clock->wall_now());
      })),
      entries_(std::move(entries)),
      actuation_(*clock_) {
  for (auto& e : entries_) by_name_.emplace(e->spec.name, e.get());
}

SwitchManager::~SwitchManager() = default;

SwitchManager::Entry* SwitchManager::find(std::string_view name) const {
  auto it = by_name_.find(name);
  return it == by_name_.end() ? nullptr : it->second;
}

Result<void> SwitchManager::actuate(std::string_view name, SwitchOp op, std::string_view actor) {
  return command(name, op, &actor);
}

Result<void> SwitchManager::restore(std::string_view name, SwitchOp op) { return command(name, op, nullptr); }

// `actor` null: a restore, which the lock and ownership do not stop.
Result<void> SwitchManager::command(std::string_view name, SwitchOp op, const std::string_view* actor) {
  Entry* e = find(name);
  if (!e) {
    Error error{ErrorKind::Config, "unknown switch '" + std::string(name) + "'", std::string(name)};
    if (bus_) bus_->publish(ActuationFailed{std::string(name), error, clock_->now()});
    return fail(std::move(error));
  }

  std::lock_guard serial(actuation_);
  Result<void> allowed;
  {
    std::lock_guard lk(state_);
    if (actor) allowed = check_access(*e, *actor);
    if (allowed && op == SwitchOp::Open) allowed = check_interlocks(*e);
  }
  // Published outside state_ so handlers may query the manager.
  if (!allowed) return failed(*e, std::move(allowed).error());

  const auto target = target_of(op);
  if (e->spec.kind == SwitchKind::ManualValve) {
    record(*e, target);
    count(*e, op == SwitchOp::Open ? &SwitchStats::opens : &SwitchStats::closes);
    if (bus_) bus_->publish(ValveChanged{e->spec.name, target, clock_->now()});
    return {};
  }
  return drive(*e, op);
}

Result<void> SwitchManager::check_access(const Entry& e, std::string_view actor) const {
  if (e.locked) return fail(ErrorKind::Interlock, "'" + e.spec.name + "' is software locked", e.spec.name);
  if (!e.owner.empty() && e.owner != actor) {
    return fail(ErrorKind::Interlock,
                "'" + e.spec.name + "' is owned by '" + e.owner + "', not '" + std::string(actor) + "'",
                e.spec.name);
  }
  return {};
}

Result<void> SwitchManager::check_interlocks(const Entry& e) const {
  for (const Entry* o : e.exclusive) {
    if (o->state != ValveState::Closed) {
      return fail(ErrorKind::Interlock,
                  "cannot open '" + e.spec.name + "': interlocked with '" + o->spec.name + "' which is " +
                      std::string(pychron::to_string(o->state)),
                  e.spec.name);
    }
  }
  for (const Entry* p : e.positive) {
    if (p->state != ValveState::Open) {
      return fail(ErrorKind::Interlock,
                  "cannot open '" + e.spec.name + "': requires '" + p->spec.name + "' open, it is " +
                      std::string(pychron::to_string(p->state)),
                  e.spec.name);
    }
  }
  return {};
}

Result<ValveState> SwitchManager::read_back(const Entry& e) const {
  const auto& source = e.spec.state_source;
  auto read = source ? e.reader->read(source->address) : e.actuator->read(e.spec.address);
  if (!read) return read;
  const bool flip = source ? source->inverted : e.spec.inverted;
  return flip ? inverse(*read) : *read;
}

Result<void> SwitchManager::drive(Entry& e, SwitchOp op) {
  const auto target = target_of(op);
  const bool channel_open = (op == SwitchOp::Open) != e.spec.inverted;
  auto sent = channel_open ? e.actuator->open(e.spec.address) : e.actuator->close(e.spec.address);
  if (!sent) {
    // The command may or may not have reached the valve.
    record(e, ValveState::Unknown);
    count(e, &SwitchStats::failures);
    return failed(e, std::move(sent).error());
  }

  settle(e.spec.settle);

  if (!e.spec.verify) {
    record(e, target);
    count(e, op == SwitchOp::Open ? &SwitchStats::opens : &SwitchStats::closes);
    if (bus_) bus_->publish(ValveChanged{e.spec.name, target, clock_->now()});
    return {};
  }

  auto read = read_back(e);
  if (!read) {
    record(e, ValveState::Unknown);
    count(e, &SwitchStats::failures);
    return failed(e, std::move(read).error());
  }
  const bool changed = record(e, *read);
  // Counted before the event goes out, so a handler that asks sees it.
  count(e, *read != target ? &SwitchStats::failures
                           : op == SwitchOp::Open ? &SwitchStats::opens : &SwitchStats::closes);
  if (*read != target) {
    if (changed && bus_) bus_->publish(ValveChanged{e.spec.name, *read, clock_->now()});
    return failed(e, Error{ErrorKind::Protocol,
                           "'" + e.spec.name + "' read back " + std::string(pychron::to_string(*read)) +
                               " after " + std::string(to_string(op)),
                           e.spec.name});
  }
  if (bus_) bus_->publish(ValveChanged{e.spec.name, *read, clock_->now()});
  return {};
}

void SwitchManager::settle(Duration d) const { clock_->sleep_for(d); }

bool SwitchManager::record(Entry& e, ValveState s) {
  std::lock_guard lk(state_);
  const ValveState was = std::exchange(e.state, s);
  if (was == s) return false;
  const WallTime now = wall_();
  if (was == ValveState::Open && e.stats.since && now > *e.stats.since) e.stats.open_time += now - *e.stats.since;
  // Out of Unknown is a reading, not a change: when it got there is not known.
  e.stats.since = was == ValveState::Unknown ? std::nullopt : std::optional<WallTime>(now);
  return true;
}

void SwitchManager::count(Entry& e, std::int64_t SwitchStats::* what) {
  std::lock_guard lk(state_);
  ++(e.stats.*what);
  if (what != &SwitchStats::failures) e.stats.last_actuation = wall_();
}

void SwitchManager::seed_stats(std::string_view name, SwitchStats stats) {
  Entry* e = find(name);
  if (!e) return;
  std::lock_guard lk(state_);
  e->stats = stats;
}

Result<void> SwitchManager::failed(const Entry& e, Error error) {
  if (bus_) bus_->publish(ActuationFailed{e.spec.name, error, clock_->now()});
  return fail(std::move(error));
}

Result<void> SwitchManager::refresh() {
  std::lock_guard serial(actuation_);
  // Each read-back device answers for all of its switches at once
  // (read_many: one request per coil run on a PLC), in config order.
  std::vector<std::pair<IValveActuator*, std::vector<Entry*>>> by_device;
  for (auto& e : entries_) {
    if (!e->actuator || !e->spec.verify) continue;
    IValveActuator* device = e->spec.state_source ? e->reader : e->actuator;
    auto group = std::find_if(by_device.begin(), by_device.end(), [&](const auto& g) { return g.first == device; });
    if (group == by_device.end()) group = by_device.insert(by_device.end(), {device, {}});
    group->second.push_back(e.get());
  }
  std::map<const Entry*, Result<ValveState>> reads;
  for (const auto& [device, group] : by_device) {
    std::vector<ValveAddress> addresses;
    for (const Entry* e : group) addresses.push_back(e->spec.state_source ? e->spec.state_source->address : e->spec.address);
    auto states = device->read_many(addresses);
    for (std::size_t i = 0; i < group.size(); ++i) {
      const Entry& e = *group[i];
      if (i >= states.size()) {
        reads.emplace(&e, fail(ErrorKind::Protocol, "read-back device answered for fewer switches", e.spec.name));
        continue;
      }
      auto read = std::move(states[i]);
      const bool flip = e.spec.state_source ? e.spec.state_source->inverted : e.spec.inverted;
      if (read && flip) read = inverse(*read);
      reads.emplace(&e, std::move(read));
    }
  }

  std::vector<Error> errors;
  for (auto& e : entries_) {
    auto it = reads.find(e.get());
    if (it == reads.end()) continue;
    auto& read = it->second;
    const auto s = read ? *read : ValveState::Unknown;
    if (!read) errors.push_back(std::move(read).error());
    if (record(*e, s) && bus_) bus_->publish(ValveChanged{e->spec.name, s, clock_->now()});
  }
  if (errors.empty()) return {};
  std::vector<std::string> lines;
  for (const auto& err : errors) lines.push_back(pychron::to_string(err));
  return fail(errors.front().kind, join(lines), errors.front().device);
}

Result<void> SwitchManager::lock(std::string_view name) {
  Entry* e = find(name);
  if (!e) return fail(ErrorKind::Config, "unknown switch '" + std::string(name) + "'");
  std::lock_guard lk(state_);
  e->locked = true;
  return {};
}

Result<void> SwitchManager::unlock(std::string_view name) {
  Entry* e = find(name);
  if (!e) return fail(ErrorKind::Config, "unknown switch '" + std::string(name) + "'");
  std::lock_guard lk(state_);
  e->locked = false;
  return {};
}

Result<void> SwitchManager::claim(std::string_view name, std::string_view actor) {
  Entry* e = find(name);
  if (!e) return fail(ErrorKind::Config, "unknown switch '" + std::string(name) + "'");
  std::lock_guard lk(state_);
  if (!e->owner.empty() && e->owner != actor) {
    return fail(ErrorKind::Interlock, "'" + e->spec.name + "' is already owned by '" + e->owner + "'",
                e->spec.name);
  }
  e->owner = std::string(actor);
  return {};
}

Result<void> SwitchManager::release(std::string_view name, std::string_view actor) {
  Entry* e = find(name);
  if (!e) return fail(ErrorKind::Config, "unknown switch '" + std::string(name) + "'");
  std::lock_guard lk(state_);
  if (e->owner != actor) {
    return fail(ErrorKind::Interlock, "'" + e->spec.name + "' is not owned by '" + std::string(actor) + "'",
                e->spec.name);
  }
  e->owner.clear();
  return {};
}

Result<ValveState> SwitchManager::state(std::string_view name) const {
  Entry* e = find(name);
  if (!e) return fail(ErrorKind::Config, "unknown switch '" + std::string(name) + "'");
  std::lock_guard lk(state_);
  return e->state;
}

Result<SwitchInfo> SwitchManager::info(std::string_view name) const {
  Entry* e = find(name);
  if (!e) return fail(ErrorKind::Config, "unknown switch '" + std::string(name) + "'");
  std::lock_guard lk(state_);
  return e->info();
}

std::vector<SwitchInfo> SwitchManager::list() const {
  std::lock_guard lk(state_);
  std::vector<SwitchInfo> out;
  out.reserve(entries_.size());
  for (const auto& e : entries_) out.push_back(e->info());
  return out;
}

std::map<std::string, ValveState> SwitchManager::states() const {
  std::lock_guard lk(state_);
  std::map<std::string, ValveState> out;
  for (const auto& e : entries_) out.emplace(e->spec.name, e->state);
  return out;
}

bool SwitchManager::contains(std::string_view name) const { return find(name) != nullptr; }

}  // namespace pychron::systems
