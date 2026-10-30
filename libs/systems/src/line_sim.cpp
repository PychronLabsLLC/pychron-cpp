#include "pychron/systems/line_sim.hpp"

#include <string>
#include <system_error>
#include <utility>

#include "pychron/sim/sim_config.hpp"
#include "pychron/systems/network_graph.hpp"

namespace pychron::systems {

namespace {

// What a stage is to the gas in it.
sim::SimRole role_of(canvas::SourceKind kind) {
  switch (kind) {
    case canvas::SourceKind::Pump: return sim::SimRole::Pump;
    case canvas::SourceKind::Getter: return sim::SimRole::Getter;
    case canvas::SourceKind::Tank: return sim::SimRole::Tank;
    case canvas::SourceKind::Pipette: return sim::SimRole::Pipette;
    case canvas::SourceKind::Spectrometer: return sim::SimRole::Spectrometer;
    case canvas::SourceKind::Laser:
    case canvas::SourceKind::None: return sim::SimRole::Plain;
  }
  return sim::SimRole::Plain;
}

// The pipe between two valves joined directly: named for the two, in byte
// order (as std::string compares: "B" before "a"), so that it is the one
// name whichever valve is asked. `kPipeMark` is in no valve's name
// (build_line_sim refuses one), so two pairs cannot make one name.
constexpr char kPipeMark = '~';
std::string pipe_between(const std::string& a, const std::string& b) {
  return a < b ? a + kPipeMark + b : b + kPipeMark + a;
}

// Whether two valves are both joined to one volume: the two valve ends of a
// tee (or of a cross) whose other end is that volume. The junction is then
// the volume's, and there is no pipe of their own between the valves.
bool share_a_volume(const NetworkGraph& graph, const std::string& a, const std::string& b) {
  const auto beside_b = graph.neighbors(b);
  for (const auto& n : graph.neighbors(a)) {
    if (!graph.is_valve(n) && beside_b.contains(n)) return true;
  }
  return false;
}

// Sim topology from the canvas plumbing. Every volume of the graph (a stage,
// a pipette, a gauge) is a volume, with the stage's size (cc) where it gives
// one and its kind as its role; a `[[pipette]]` is a pipette, wired as it is
// drawn, and a `[[gauge]]` a gauge.
//
// A canvas draws pipe straight from one valve to the next. A valve stands
// between two volumes, so each such pair gets the pipe as a small volume of
// its own, `<a>~<b>`, between them (after the canvas's volumes: one drawn
// with that name is the volume). The exception is a pair that a tee joins
// to each other and to one volume: both are on that volume already.
sim::SimTopology topology_of(const NetworkGraph& graph, const canvas::Canvas& canvas) {
  sim::SimTopology t;
  for (const auto& v : graph.volumes()) {
    sim::SimVolume volume{v};  // no size: the simulator's default for its role
    for (const auto& g : canvas.gauges) {
      if (g.name == v) volume.role = sim::SimRole::Gauge;
    }
    for (const auto& s : canvas.stages) {
      if (s.name != v) continue;
      if (s.volume) volume.cc = *s.volume;
      volume.role = role_of(canvas::source_kind(s));
    }
    for (const auto& p : canvas.pipettes) {
      if (p.name == v) volume.role = sim::SimRole::Pipette;
    }
    t.volumes.push_back(std::move(volume));
  }
  for (const auto& v : graph.valves()) t.valves.push_back(v);
  for (const auto& n : graph.volumes()) {
    for (const auto& m : graph.neighbors(n)) t.edges.emplace_back(n, m);
  }
  for (const auto& n : graph.valves()) {
    for (const auto& m : graph.neighbors(n)) {
      if (!graph.is_valve(m)) {
        t.edges.emplace_back(n, m);
      } else if (!share_a_volume(graph, n, m)) {
        const std::string pipe = pipe_between(n, m);
        if (n < m) t.volumes.push_back({pipe, 0.0, sim::SimRole::Pipe});  // once for the pair
        t.edges.emplace_back(n, pipe);
      }
    }
  }
  return t;
}

}  // namespace

SimFileBeside sim_file_beside(const std::filesystem::path& system_file, bool has_canvas) {
  // Not named: the sim.toml beside the line, if the lab has one. Its names
  // are the canvas's, so it is the lab's only when the canvas is loaded;
  // without, a simulated line says that it left the file alone.
  SimFileBeside out;
  const auto beside = std::filesystem::path(system_file).parent_path() / "sim.toml";
  std::error_code ec;
  if (std::filesystem::is_regular_file(beside, ec)) {
    if (has_canvas) {
      out.read = beside;
    } else {
      out.skipped = beside;
    }
  }
  return out;
}

Result<LineSim> build_line_sim(const config::SystemConfig& config, const canvas::Canvas* canvas, const Clock& clock,
                               LineSimOptions options) {
  LineSim out;
  sim::SimTopology topology;
  if (canvas != nullptr) topology = topology_of(NetworkGraph::from_canvas(*canvas), *canvas);
  // A pipe is named `<a>~<b>`: with the mark in a valve's own name, valves
  // `A`, `B~C` and `A~B`, `C` would make one pipe name between them.
  for (const auto& valve : topology.valves) {
    if (valve.find(kPipeMark) != std::string::npos) {
      return fail(ErrorKind::Config, "sim: valve '" + valve + "': a valve's name may not contain '" +
                                         std::string(1, kPipeMark) +
                                         "' on a simulated line (the pipe between two valves joined directly is "
                                         "named '<a>~<b>')");
    }
  }

  // The same line file serves a tool that loads no canvas (elctl laser): the
  // lab its sim file describes is not there to be given numbers. Said once,
  // for the file `[sim] file` names and for the one beside the line alike.
  std::filesystem::path file;
  std::filesystem::path skipped;
  if (options.sim_file) {
    file = *options.sim_file;
  } else if (!config.sim.file.empty()) {
    (canvas != nullptr ? file : skipped) = std::filesystem::path(config.source_file).parent_path() / config.sim.file;
  } else {
    skipped = options.sim_beside_skipped;
  }
  if (!skipped.empty()) {
    out.notes.push_back(
        {LogLevel::Info, "sim: " + skipped.generic_string() + " not read: the line was loaded without its canvas"});
  }
  sim::SimSettings settings = std::move(options.sim);
  if (!file.empty()) {
    // A gauge off the canvas is a volume of its own once its controller is
    // simulated, so the file may name one.
    sim::SimTopology known = topology;
    for (const auto& g : config.gauges) known.volumes.push_back({g.name, 0.0, sim::SimRole::Gauge});
    auto loaded = sim::load_sim_settings(file, known, std::move(settings));
    if (!loaded) return fail(loaded.error());
    settings = std::move(*loaded);
  }
  // A cryotrap's cryostat is a Lake Shore driver of this line.
  for (const auto& trap : settings.cryo_traps) {
    const auto driver = config.drivers.find(trap.cryostat);
    if (driver == config.drivers.end() || driver->second.kind != "lakeshore")
      return fail(ErrorKind::Config, (settings.file.empty() ? std::string("sim settings") : settings.file) +
                                         ": cryo_trap on '" + trap.volume + "': cryostat '" + trap.cryostat +
                                         "' is not a lakeshore driver of " + config.source_file);
  }

  out.system = std::make_unique<sim::SimSystem>(clock, topology, std::move(settings));
  if (auto refused = out.system->build_error()) return fail(*refused);

  const auto source = out.system->spectrometer_volume();
  for (const auto& v : topology.volumes) {
    if (v.role == sim::SimRole::Spectrometer && source && v.name != *source) {
      out.notes.push_back({LogLevel::Warn, "sim: stage '" + v.name +
                                               "' is a second spectrometer; the simulated source is '" + *source +
                                               "'"});
    }
  }
  if (const auto without = out.system->valves_without_physics(); !without.empty()) {
    std::string said = "sim: " + std::to_string(without.size()) + " valve(s) carry no gas in the simulation: ";
    for (std::size_t i = 0; i < without.size(); ++i) {
      said += (i == 0 ? "" : ", ") + without[i].first + " (" + without[i].second + ")";
    }
    out.notes.push_back({LogLevel::Info, std::move(said)});
  }
  return out;
}

}  // namespace pychron::systems
