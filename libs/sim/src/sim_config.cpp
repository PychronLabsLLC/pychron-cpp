#include "pychron/sim/sim_config.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <toml++/toml.hpp>

#include "pychron/core/config/diagnostic.hpp"
#include "pychron/core/config/located.hpp"
#include "pychron/core/error.hpp"
#include "pychron/sim/gas.hpp"

namespace pychron::sim {
namespace {

using config::Diagnostic;
using config::SourceLoc;

// The closed range a number must lie in, and what it is a number of;
// `open_low`: above `low`, not at it.
struct Range {
  double low = 0.0;
  double high = 0.0;
  std::string_view unit;
  bool open_low = false;
};

constexpr Range kPressure{0.0, 1e4, "mbar"};
constexpr Range kSize{1e-6, 1e9, "cc"};
constexpr Range kFlow{0.0, 1e9, "L/s"};  // conductances and pump speeds
constexpr Range kGasRate{0.0, 1e3, "mbar L/s"};  // outgassing, leaks
constexpr Range kRate{0.0, 1e6, "1/s"};
constexpr Range kSensitivity{0.0, 1e30, "fA per mbar", true};
constexpr Range kNoise{0.0, 10.0, "as a fraction of the reading"};
constexpr Range kRatio{0.0, 1e9, "as a ratio to Ar36"};
constexpr Range kMemory{0.0, 1e30, "fA/s"};
constexpr Range kBaseline{-1e30, 1e30, "fA (cps on a counting detector)"};  // finite, either sign
// The most the memory may be as gas, through the sensitivity.
constexpr Range kMemoryAsGas{0.0, 1e3, "mbar/s"};

// How many names a message lists before it only counts them.
constexpr std::size_t kNamesListed = 12;

std::string shown(double value) {
  std::ostringstream out;
  out << value;
  return out.str();
}

std::string described(const Range& range) {
  return std::string(range.open_low ? "above " : "from ") + shown(range.low) + " to " + shown(range.high) + " " +
         std::string(range.unit);
}

// What would have been right, after a name or a key that is not: the
// candidates, while they are few enough to read, else how many there are.
template <typename Names>
std::string known(const Names& names) {
  const std::size_t count = static_cast<std::size_t>(std::distance(std::begin(names), std::end(names)));
  if (count == 0) return "; there is none";
  if (count > kNamesListed) return "; " + std::to_string(count) + " are known";
  std::string out = "; known: ";
  bool first = true;
  for (const auto& name : names) {
    out += (first ? "" : ", ") + std::string(name);
    first = false;
  }
  return out;
}

// `air` and `cocktail`; a [compositions.*] table of either name goes first.
std::optional<Composition> built_in(std::string_view name) {
  if (name == "air") return air_ratios();
  if (name == "cocktail") return cocktail_ratios();
  return std::nullopt;
}

// Reads one file's tables, gathering every problem with where it is.
class Reader {
 public:
  Reader(std::string file, const SimTopology& topology, SimSettings base)
      : file_(std::move(file)), settings_(std::move(base)) {
    for (const auto& v : topology.volumes) roles_.emplace(v.name, v.role);  // the first of a name
    for (const auto& v : topology.valves) valves_.insert(v);
  }

  Result<SimSettings> read(const toml::table& root) {
    reject_unknown(root, "",
                   {"defaults", "compositions", "volumes", "valves", "pumps", "spectrometer", "detectors"});
    if (const auto* t = table(root, "defaults")) read_defaults(*t);
    // Compositions before the volumes that name them.
    each_named(root, "compositions", [this](const std::string& path, const std::string& name, const toml::table& t) {
      read_composition(path, name, t);
    });
    each_named(root, "volumes", [this](const std::string& path, const std::string& name, const toml::table& t) {
      read_volume(path, name, t);
    });
    each_named(root, "valves", [this](const std::string& path, const std::string& name, const toml::table& t) {
      read_valve(path, name, t);
    });
    each_named(root, "pumps", [this](const std::string& path, const std::string& name, const toml::table& t) {
      read_pump(path, name, t);
    });
    if (const auto* t = table(root, "spectrometer")) read_spectrometer(*t);
    each_named(root, "detectors", [this](const std::string& path, const std::string& name, const toml::table& t) {
      read_detector(path, name, t);
    });
    if (!problems_.empty()) return fail(config::to_error(problems_));
    return std::move(settings_);
  }

 private:
  SourceLoc loc(const toml::node& node) const {
    return SourceLoc{file_, node.source().begin.line, node.source().begin.column};
  }

  void problem(const toml::node& node, std::string key, std::string message) {
    problems_.push_back({loc(node), std::move(key), std::move(message)});
  }

  static std::string join(const std::string& path, std::string_view key) {
    return path.empty() ? std::string(key) : path + "." + std::string(key);
  }

  void reject_unknown(const toml::table& t, const std::string& path,
                      std::initializer_list<std::string_view> known_keys) {
    for (auto&& [key, node] : t) {
      bool found = false;
      for (const auto k : known_keys) found = found || k == key.str();
      if (!found) problem(node, join(path, key.str()), "unknown key" + known(known_keys));
    }
  }

  // The table at `key` of `parent`; nothing, with a problem, if it is there
  // and is no table.
  const toml::table* table(const toml::table& parent, std::string_view key) {
    const auto* node = parent.get(key);
    if (node == nullptr) return nullptr;
    if (const auto* t = node->as_table()) return t;
    problem(*node, std::string(key), "expected a table");
    return nullptr;
  }

  // Each `[section.<name>]`.
  void each_named(const toml::table& root, std::string_view section,
                  const std::function<void(const std::string&, const std::string&, const toml::table&)>& each) {
    const auto* names = table(root, section);
    if (names == nullptr) return;
    for (auto&& [key, node] : *names) {
      const std::string name(key.str());
      const std::string path = join(std::string(section), name);
      if (const auto* t = node.as_table()) {
        each(path, name, *t);
      } else {
        problem(node, path, "expected a table");
      }
    }
  }

  // The number at `key`, within `range`; nothing if it is not there, and
  // nothing with a problem if it is no number or not one the line can have.
  std::optional<double> number(const toml::table& t, const std::string& path, std::string_view key,
                               const Range& range) {
    const auto* node = t.get(key);
    if (node == nullptr) return std::nullopt;
    double value = 0.0;
    if (const auto* f = node->as_floating_point()) {
      value = f->get();
    } else if (const auto* i = node->as_integer()) {
      value = static_cast<double>(i->get());
    } else {
      problem(*node, join(path, key), "expected a number");
      return std::nullopt;
    }
    if (!std::isfinite(value)) {
      problem(*node, join(path, key), "must be a finite number");
      return std::nullopt;
    }
    if (value < range.low || value > range.high || (range.open_low && !(value > range.low))) {
      problem(*node, join(path, key), "value " + shown(value) + " out of range, must be " + described(range));
      return std::nullopt;
    }
    return value;
  }

  void read_defaults(const toml::table& t) {
    const std::string path = "defaults";
    reject_unknown(t, path,
                   {"pressure", "volume_cc", "pipe_cc", "gauge_cc", "valve_conductance", "outgassing", "noise", "seed"});
    if (auto v = number(t, path, "pressure", kPressure)) settings_.default_pressure = *v;
    if (auto v = number(t, path, "volume_cc", kSize)) settings_.default_volume_cc = *v;
    if (auto v = number(t, path, "pipe_cc", kSize)) settings_.pipe_cc = *v;
    if (auto v = number(t, path, "gauge_cc", kSize)) settings_.gauge_cc = *v;
    if (auto v = number(t, path, "valve_conductance", kFlow)) settings_.valve_conductance = *v;
    if (auto v = number(t, path, "outgassing", kGasRate)) settings_.outgassing = *v;
    if (auto v = number(t, path, "noise", kNoise)) settings_.noise = *v;
    if (const auto* node = t.get("seed")) {
      const auto* seed = node->as_integer();
      if (seed == nullptr || seed->get() < 0) {
        problem(*node, join(path, "seed"), "expected an integer of zero or more");
      } else {
        settings_.seed = static_cast<std::uint64_t>(seed->get());
      }
    }
  }

  void read_composition(const std::string& path, const std::string& name, const toml::table& t) {
    Composition ratios{};
    for (auto&& [key, node] : t) {
      std::size_t species = kSpeciesCount;
      for (std::size_t s = 0; s < kSpeciesCount; ++s) {
        if (kSpeciesName[s] == key.str()) species = s;
      }
      if (species == kSpeciesCount) {
        problem(node, join(path, key.str()), "unknown key" + known(kSpeciesName));
      } else if (auto v = number(t, path, key.str(), kRatio)) {
        ratios[species] = *v;
      }
    }
    settings_.named[name] = ratios;
  }

  void read_volume(const std::string& path, const std::string& name, const toml::table& t) {
    const auto role = roles_.find(name);
    if (role == roles_.end()) {
      problem(t, path, "unknown volume '" + name + "'" + known(volume_names()));
      return;
    }
    reject_unknown(t, path, {"composition", "argon40", "pressure", "leak", "volume_cc"});
    if (auto v = number(t, path, "leak", kGasRate)) settings_.leaks[name] = *v;
    if (auto v = number(t, path, "volume_cc", kSize)) settings_.sizes[name] = *v;

    // What it holds. The ratios first: those named, else air.
    std::optional<Composition> ratios;
    const auto* named = t.get("composition");
    if (named != nullptr) {
      const auto* text = named->as_string();
      if (text == nullptr) {
        problem(*named, join(path, "composition"), "expected a string");
        return;
      }
      if (auto own = settings_.named.find(text->get()); own != settings_.named.end()) {
        ratios = own->second;
      } else if (auto given = built_in(text->get())) {
        ratios = *given;
      } else {
        std::set<std::string, std::less<>> names{"air", "cocktail"};
        for (const auto& [own_name, ratios_of] : settings_.named) names.insert(own_name);
        problem(*named, join(path, "composition"), "unknown composition '" + text->get() + "'" + known(names));
        return;
      }
    }
    const auto* ar40_node = t.get("argon40");
    const auto* pressure_node = t.get("pressure");
    const auto ar40 = number(t, path, "argon40", kPressure);
    const auto pressure = number(t, path, "pressure", kPressure);
    if (ar40_node != nullptr && pressure_node != nullptr) {
      problem(*pressure_node, join(path, "pressure"), "give argon40 or pressure, not both");
      return;
    }
    if ((ar40_node != nullptr && !ar40) || (pressure_node != nullptr && !pressure)) return;  // reported above
    if (named == nullptr && !ar40 && !pressure) return;                                      // nothing said of its gas

    if (!ratios && pressure) {
      // That much air: as a test would set it in code.
      settings_.initial_pressures[name] = *pressure;
      settings_.compositions.erase(name);
      return;
    }
    const Composition proportions = ratios ? *ratios : air_ratios();
    const toml::node& where = ar40 ? *ar40_node : pressure ? *pressure_node : *named;
    const std::string_view key = ar40 ? "argon40" : pressure ? "pressure" : "composition";
    Composition held{};
    if (ar40 || (!pressure && role->second == SimRole::Tank)) {
      if (!(proportions[index(Species::Ar40)] > 0.0)) {
        problem(where, join(path, key), "the composition has no Ar40 to set a pressure of");
        return;
      }
      held = with_ar40(proportions, ar40 ? *ar40 : settings_.tank_argon40);
    } else {
      if (!(total(proportions) > 0.0)) {
        problem(where, join(path, key), "the composition is of nothing");
        return;
      }
      held = scaled(proportions, (pressure ? *pressure : settings_.default_pressure) / total(proportions));
    }
    for (std::size_t s = 0; s < kSpeciesCount; ++s) {
      if (!std::isfinite(held[s]) || held[s] > kPressure.high) {
        problem(where, join(path, key),
                "gives " + shown(held[s]) + " mbar of " + std::string(kSpeciesName[s]) + ", must be " +
                    described(kPressure));
        return;
      }
    }
    settings_.compositions[name] = held;
    settings_.initial_pressures.erase(name);
  }

  void read_valve(const std::string& path, const std::string& name, const toml::table& t) {
    if (!valves_.contains(name) || roles_.contains(name)) {  // a valve with a volume's name is not there
      std::set<std::string, std::less<>> names;
      for (const auto& valve : valves_) {
        if (!roles_.contains(valve)) names.insert(valve);
      }
      problem(t, path, "unknown valve '" + name + "'" + known(names));
      return;
    }
    reject_unknown(t, path, {"conductance"});
    if (auto v = number(t, path, "conductance", kFlow)) settings_.conductances[name] = *v;
  }

  void read_pump(const std::string& path, const std::string& name, const toml::table& t) {
    if (!roles_.contains(name)) {
      problem(t, path, "unknown volume '" + name + "' for a pump" + known(volume_names()));
      return;
    }
    reject_unknown(t, path, {"speed", "base"});
    const auto speed = number(t, path, "speed", kFlow);
    const auto base = number(t, path, "base", kPressure);
    // Over the pump the base has on that volume, a key that is not there
    // leaving what it had (a pump given by its time constant keeps that);
    // with none there, over a pump stage's.
    const auto had = settings_.pumps.find(name);
    const bool fresh = had == settings_.pumps.end();
    SimPump pump;
    if (fresh) {
      pump.base = settings_.pump_base;
    } else {
      pump = had->second;
    }
    if (base) pump.base = *base;
    settings_.pumps[name] = pump;
    if (speed) {
      settings_.pump_speeds[name] = *speed;
    } else if (fresh) {
      settings_.pump_speeds[name] = settings_.pump_speed;
    }
  }

  void read_spectrometer(const toml::table& t) {
    const std::string path = "spectrometer";
    reject_unknown(t, path, {"sensitivity", "consumption", "memory_fA_per_s"});
    if (auto v = number(t, path, "sensitivity", kSensitivity)) settings_.source.sensitivity = *v;
    if (auto v = number(t, path, "consumption", kRate)) settings_.source.consumption = *v;
    if (auto v = number(t, path, "memory_fA_per_s", kMemory)) settings_.source.memory_fa_per_s = *v;
    // As gas: fA/s over fA/mbar, whichever of the two the file gives. Out
    // of range, the key at fault is the memory if the file has it, else the
    // sensitivity it gives.
    const double mbar_per_s = settings_.source.memory_fa_per_s / settings_.source.sensitivity;
    if (!(mbar_per_s <= kMemoryAsGas.high)) {
      const auto* memory = t.get("memory_fA_per_s");
      const auto* sensitivity = t.get("sensitivity");
      if (memory == nullptr && sensitivity != nullptr) {
        problem(*sensitivity, join(path, "sensitivity"),
                "the memory (" + shown(settings_.source.memory_fa_per_s) + " fA/s) is then " + shown(mbar_per_s) +
                    " mbar/s, must be " + described(kMemoryAsGas));
      } else {
        problem(memory != nullptr ? *memory : static_cast<const toml::node&>(t), join(path, "memory_fA_per_s"),
                "is " + shown(mbar_per_s) + " mbar/s at this sensitivity, must be " + described(kMemoryAsGas));
      }
    }
  }

  void read_detector(const std::string& path, const std::string& name, const toml::table& t) {
    reject_unknown(t, path, {"baseline", "baseline_drift_per_h"});
    SimSettings::DetectorBaseline detector;
    if (auto found = settings_.detectors.find(name); found != settings_.detectors.end()) detector = found->second;
    if (auto v = number(t, path, "baseline", kBaseline)) detector.baseline = *v;
    if (auto v = number(t, path, "baseline_drift_per_h", kBaseline)) detector.drift_per_h = *v;
    settings_.detectors[name] = detector;
  }

  std::vector<std::string_view> volume_names() const {
    std::vector<std::string_view> names;
    for (const auto& [name, role] : roles_) names.push_back(name);
    return names;
  }

  std::string file_;
  SimSettings settings_;
  std::map<std::string, SimRole, std::less<>> roles_;  // by volume
  std::set<std::string, std::less<>> valves_;
  std::vector<Diagnostic> problems_;
};

}  // namespace

Result<SimSettings> load_sim_settings(const std::filesystem::path& file, const SimTopology& topology,
                                      SimSettings base) {
  const std::string name = file.generic_string();
  std::ifstream in(file, std::ios::binary);
  if (!in) return fail(config::to_error({{SourceLoc{name, 0, 0}, "file", "cannot read sim file"}}));
  std::ostringstream text;
  text << in.rdbuf();

  auto parsed = toml::parse(text.str(), name);
  if (!parsed) {
    const auto& error = parsed.error();
    return fail(config::to_error({{SourceLoc{name, error.source().begin.line, error.source().begin.column}, "toml",
                                   "syntax error: " + std::string(error.description())}}));
  }
  return Reader(name, topology, std::move(base)).read(parsed.table());
}

}  // namespace pychron::sim
