#include "pychron/core/config/validate.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <variant>

namespace pychron::config {
namespace {

class Validator {
 public:
  explicit Validator(const SystemConfig& c) : c_(c) {}

  std::vector<Diagnostic> run() {
    check_drivers();
    collect_valve_names();
    check_valves();
    check_positive_cycles();
    check_gauges();
    check_pipettes();
    check_aliases();
    return std::move(out_);
  }

 private:
  void report(const Located& e, const std::string& field, std::string message) {
    out_.push_back({e.where(field), e.path + "." + field, std::move(message)});
  }

  void check_drivers() {
    for (const auto& [name, d] : c_.drivers) {
      if (!c_.transports.contains(d.transport)) {
        report(d, "transport", "unknown transport '" + d.transport + "'");
      }
    }
  }

  void collect_valve_names() {
    auto claim = [&](const Located& e, const std::string& name) {
      if (!all_valves_.insert(name).second) report(e, "name", "duplicate valve name '" + name + "'");
    };
    for (const auto& v : c_.valves) claim(v, v.name);
    for (const auto& m : c_.manual_valves) claim(m, m.name);
    for (const auto& s : c_.switches) claim(s, s.name);
  }

  // A state source names a driver, and replaces the read-back verify=false
  // turns off.
  template <class S>
  void check_actuation(const S& s) {
    if (!s.state_source) return;
    if (!c_.drivers.contains(s.state_source->driver)) {
      report(*s.state_source, "driver", "unknown driver '" + s.state_source->driver + "'");
    }
    if (!s.verify) report(s, "verify", "verify = false reads nothing back; it cannot have a state_source");
  }

  // Switches share actuators and the (actuator, address) space with valves.
  void check_switches(std::map<std::pair<std::string, std::string>, std::string>& addresses) {
    for (const auto& s : c_.switches) {
      if (!c_.drivers.contains(s.actuator)) {
        report(s, "actuator", "unknown actuator '" + s.actuator + "'");
      }
      auto [it, inserted] = addresses.emplace(std::make_pair(s.actuator, s.address), s.name);
      if (!inserted) {
        report(s, "address",
               "address '" + s.address + "' on actuator '" + s.actuator + "' already used by '" + it->second + "'");
      }
      check_actuation(s);
    }
  }

  void check_interlock_list(const ValveConfig& v, const std::vector<std::string>& list, const char* key) {
    for (std::size_t i = 0; i < list.size(); ++i) {
      const auto field = std::string(key) + "[" + std::to_string(i) + "]";
      const auto& other = list[i];
      if (other == v.name) {
        report(v, field, "valve '" + v.name + "' cannot interlock with itself");
      } else if (!all_valves_.contains(other)) {
        report(v, field, "unknown valve '" + other + "'");
      }
    }
  }

  void check_valves() {
    std::map<std::pair<std::string, std::string>, std::string> addresses;  // (actuator, address) -> valve
    for (const auto& v : c_.valves) {
      if (!c_.drivers.contains(v.actuator)) {
        report(v, "actuator", "unknown actuator '" + v.actuator + "'");
      }
      auto [it, inserted] = addresses.emplace(std::make_pair(v.actuator, v.address), v.name);
      if (!inserted) {
        report(v, "address",
               "address '" + v.address + "' on actuator '" + v.actuator + "' already used by valve '" +
                   it->second + "'");
      }
      check_actuation(v);
      check_interlock_list(v, v.interlocks, "interlocks");
      check_interlock_list(v, v.positive_interlocks, "positive_interlocks");
      for (std::size_t i = 0; i < v.positive_interlocks.size(); ++i) {
        const auto& p = v.positive_interlocks[i];
        if (std::find(v.interlocks.begin(), v.interlocks.end(), p) != v.interlocks.end()) {
          report(v, "positive_interlocks[" + std::to_string(i) + "]",
                 "valve '" + p + "' is both an interlock and a positive interlock");
        }
      }
    }
    check_switches(addresses);
  }

  // DFS over "must be open before" edges; reports each cycle once.
  void check_positive_cycles() {
    std::map<std::string, const ValveConfig*> by_name;
    for (const auto& v : c_.valves) by_name.emplace(v.name, &v);

    enum class Mark { White, Grey, Black };
    std::map<std::string, Mark> mark;
    std::vector<std::string> stack;

    auto visit = [&](auto&& self, const std::string& name) -> void {
      mark[name] = Mark::Grey;
      stack.push_back(name);
      const auto* v = by_name.at(name);
      for (std::size_t i = 0; i < v->positive_interlocks.size(); ++i) {
        const auto& next = v->positive_interlocks[i];
        if (next == name || !by_name.contains(next)) continue;  // reported elsewhere
        const auto m = mark[next];
        if (m == Mark::Grey) {
          auto from = std::find(stack.begin(), stack.end(), next);
          std::string cycle;
          for (auto s = from; s != stack.end(); ++s) cycle += *s + " -> ";
          cycle += next;
          report(*v, "positive_interlocks[" + std::to_string(i) + "]", "positive interlock cycle: " + cycle);
        } else if (m == Mark::White) {
          self(self, next);
        }
      }
      stack.pop_back();
      mark[name] = Mark::Black;
    };

    for (const auto& v : c_.valves) {
      if (mark[v.name] == Mark::White) visit(visit, v.name);
    }
  }

  void check_gauges() {
    std::set<std::string> names;
    for (const auto& g : c_.gauges) {
      if (!names.insert(g.name).second) report(g, "name", "duplicate gauge name '" + g.name + "'");
      auto d = c_.drivers.find(g.driver);
      if (d == c_.drivers.end()) {
        report(g, "driver", "unknown driver '" + g.driver + "'");
        continue;
      }
      const auto& channels = d->second.channels;
      if (!channels.empty() && std::find(channels.begin(), channels.end(), g.channel) == channels.end()) {
        report(g, "channel",
               "channel " + std::to_string(g.channel) + " is not declared by driver '" + g.driver + "'");
      }
      if (g.alarm_low && g.alarm_high && *g.alarm_low >= *g.alarm_high) {
        report(g, "alarm_low", "alarm_low must be less than alarm_high");
      }
    }
  }

  void check_pipettes() {
    std::set<std::string> names;
    std::set<std::string> real_valves;
    for (const auto& v : c_.valves) real_valves.insert(v.name);
    for (const auto& p : c_.pipettes) {
      if (!names.insert(p.name).second) report(p, "name", "duplicate pipette name '" + p.name + "'");
      if (!real_valves.contains(p.inner)) report(p, "inner", "unknown valve '" + p.inner + "'");
      if (!real_valves.contains(p.outer)) report(p, "outer", "unknown valve '" + p.outer + "'");
      if (p.inner == p.outer) report(p, "outer", "inner and outer must be different valves");
    }
  }

  // valves.* aliases are what plans open and close; they must name a valve.
  void check_aliases() {
    for (const auto& [key, a] : c_.aliases) {
      if (!key.starts_with("valves.")) continue;
      const auto* name = std::get_if<std::string>(&a.value);
      if (name == nullptr) {
        out_.push_back({a.loc, a.path, "valve alias must be a valve name (string)"});
      } else if (!all_valves_.contains(*name)) {
        out_.push_back({a.loc, a.path, "unknown valve '" + *name + "'"});
      }
    }
  }

  const SystemConfig& c_;
  std::set<std::string> all_valves_;
  std::vector<Diagnostic> out_;
};

}  // namespace

std::vector<Diagnostic> validate(const SystemConfig& config) { return Validator(config).run(); }

}  // namespace pychron::config
