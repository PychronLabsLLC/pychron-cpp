# Lab simulator: gas from the line to the detector

Date: 2026-10-06
Status: Implemented (2026-10-07, branch `feat/lab-simulator`)
Owner: Jake Ross
Builds on: `2026-09-29-instrument-control-design.md` §7 (`SimSystem`),
`2026-09-29-spectrometer-control-design.md` §8.1 (`BeamModel`),
`2026-10-06-virtual-clock-design.md` (simulated time).

This document describes what was built. Where the first draft said something
else, section 11 lists the change and why. The user guide is
`docs/simulator.md`; the plan was
`docs/superpowers/plans/2026-10-07-lab-simulator.md`.

## 1. Intent

Two simulators existed and did not know about each other.

- `SimSystem` modelled the extraction line: volumes and valves as a graph,
  one pressure per volume, instant equilibration across open valves,
  exponential pump-down, gauge noise. Every valve, gauge, heater and laser
  driver of a `kind = "sim"` config talks to it.
- `BeamModel` modelled the ion beam: peak shapes, detector noise, dead time.
  Its gas was a fixed list of abundances with one exponential rate each,
  started when the model was built.

So a simulated run measured the same signal whatever the extraction script
did. Opening the inlet changed nothing at the detector; an air shot and a
blank were the same analysis; a script that never opened the pipette looked
fine.

This spec joins them. The line carries gas by species, the spectrometer's
source is one of the line's volumes, and the beam reads that volume. The goal
is that pychron run with `--sim` behaves as it would on a lab: the signals
have the right shape at the right time for what the valves did.

For: development and UI work without hardware, demonstrations and training,
and integration tests of the executor, scripts and conditionals that run in
milliseconds under the virtual clock.

Built: air and cocktail compositions, tanks and pipettes; line blank,
outgassing and leaks; pumps and getters; the ion source's consumption and
memory; detector baselines; reproducible noise; the example lab's air queue.

Not in scope, and not built (section 10): gas released by a laser or furnace
(the model has the entry point, section 3.5); recovering a known age from a
simulated sample (amounts are plausible, not calibrated); fault injection
beyond what the device sims already do; a simulator control panel.

## 2. Shape

```
canvas + sim.toml ──> SimTopology + SimSettings
                              |
                              v
                         SimSystem ───────────────> BeamModel
                     (clock, mutex, device hooks)   (peaks, detectors, noise)
                              |        beam_gas(): the argon in the
                              v        spectrometer volume at an instant
                         GasNetwork
                     (volumes, valves, pumps; no clock)
                              |
                              v
                         LinearFlow x 6
                     (one exact solution per species)
```

- `gas.hpp`: the species and `Composition`.
- `LinearFlow`: the exact solution of one species' linear system.
- `GasNetwork` owns the state and builds the six flows. With `gas.hpp` and
  `LinearFlow` it includes nothing outside the standard library and
  `pychron/core/error.hpp`, and is tested on its own.
- `SimSystem` keeps its device hooks and its public API, describes the
  network from a topology and settings, keeps its time and guards it.
- `BeamModel` takes an optional gas provider. Without one it behaves as it
  did.
- `ExtractionLine` (`libs/systems`) turns the canvas into the topology and
  reads `sim.toml`; `feed_beam_from_line` (`libs/systems` bringup) joins a
  beam to a line.

## 3. The gas model

`libs/sim/include/pychron/sim/gas.hpp`, `linear_flow.hpp`, `gas_network.hpp`
and their sources.

### 3.1 State

A partial pressure (mbar) per species per volume. Species are fixed:

| Species | Mass | Note |
|---|---|---|
| `Ar36` `Ar37` `Ar38` `Ar39` `Ar40` | 35.968, 36.967, 37.963, 38.964, 39.962 | what the spectrometer measures |
| `active` | 28 | everything a getter removes (N2, O2, H2O, CO2), as one bulk gas |

A volume's total pressure is the sum over species. `active` is there so that
gauges read sensibly: air is about 1 % argon, and a line that has taken an air
shot and not yet gettered it must show it.

```cpp
enum class Species { Ar36, Ar37, Ar38, Ar39, Ar40, Active };
using Composition = std::array<double, kSpeciesCount>;   // mbar, mbar L, a rate, or ratios
constexpr std::size_t index(Species) noexcept;
constexpr double total(const Composition&) noexcept;
constexpr Composition scaled(const Composition&, double) noexcept;
constexpr Composition air_ratios() noexcept;       // to Ar36
constexpr Composition cocktail_ratios() noexcept;
constexpr Composition with_ar40(const Composition& ratios, double ar40) noexcept;
```

### 3.2 Elements

```cpp
struct GasVolume   { std::string name; double litres = 0.05; Composition initial{};  // mbar
                     Composition source_per_s{};         // mbar L / s: outgassing, leaks, memory
                     SpeciesRates loss_per_s{}; };       // 1 / s: first-order loss
struct GasValve    { std::string name; double conductance = 0.1; };   // L / s for Ar40
struct GasPump     { std::string volume; double speed = 50.0; double base = 1e-9;   // L / s, mbar
                     bool nobles = true; bool active = true; };
struct GasTopology { std::vector<GasVolume> volumes; std::vector<GasValve> valves;
                     std::vector<GasPump> pumps;
                     std::vector<std::pair<std::string, std::string>> edges; };
```

- A valve conducts only while open, and all start closed. Its conductance for
  a species of mass M is `conductance * sqrt(39.962 / M)` (molecular flow), so
  lighter argon arrives sooner and an inlet fractionates until it has
  equilibrated.
- Volumes joined by an edge with no valve between them are merged at build
  time: the size is the sum, the starting pressure the volume-weighted mean,
  the sources the sum, the loss the volume-weighted mean, and the whole has
  every pump of its parts. Each name still answers, with the pressure of the
  whole.
- A pump is a sink on its volume: `dp/dt = -(speed / V) (p - share)`, the
  base being shared among the species it pumps in the proportions of air.
  `nobles = false` makes a getter: it pumps `active` only. Pumps on one
  volume add.
- `source_per_s` is a constant inflow; `loss_per_s` a first-order loss. They
  are how outgassing, leaks, ion consumption and source memory are expressed
  (sections 4 and 5).
- A valve has physics when it stands between exactly two merged volumes.
  Only an edge between a valve and a volume gives the valve a neighbour; an
  edge joining two valves, or naming something that is not there, joins
  nothing. A valve with no volume, one, three or more, or the same merged
  volume on both sides is still a valve with a state, and carries nothing;
  `valves_without_physics()` names each with why.

### 3.3 Equations

For each species, with `n_i = p_i V_i`:

```
dn_i/dt = sum_j C_ij (n_j / V_j - n_i / V_i)      open valves between i and j
          - (S_i / V_i) (n_i - share_i V_i)       pumps on i
          - L_i n_i                               first-order loss in i
          + q_i                                   constant source in i
```

That is `dn/dt = K n + s` with `K` and `s` constant between valve events.
Species do not interact, so there is one small system per species (as many
unknowns as merged volumes, tens at most).

### 3.4 Solution: `LinearFlow`

```cpp
struct FlowTerms { std::vector<double> volume;                    // litres
                   struct Link { std::size_t a, b; double conductance; };
                   std::vector<Link> links;
                   std::vector<double> loss;                      // 1 / s per volume
                   std::vector<double> source; };                 // mbar L / s per volume
class LinearFlow {
 public:
  static Result<LinearFlow> make(FlowTerms terms);
  void advance(std::vector<double>& n, double dt) const;          // n in mbar L
  std::size_t size() const noexcept;
};
```

`K` is similar to a symmetric matrix `A = D^-1 K D`, `D = diag(sqrt(V))`.
`A = U L U^T` is found once per valve event and every eigenvalue is `<= 0`.
In `y = U^T D^-1 n` the equations come apart and each mode is advanced by
any `dt` at once:

```
y_k(t + dt) = y_k(t) exp(l_k dt) + g_k (exp(l_k dt) - 1) / l_k      (dt when l_k = 0)
```

How the decomposition is made matters where rates differ by many orders (a
valve that equilibrates in a microsecond beside a loss of hours):

- The mode of a group of linked volumes that loses nothing is known
  beforehand (`sqrt(V)` over the group). It is taken out exactly, never
  computed, so what such a group holds does not drift.
- The rotations are one-sided Jacobi on a factor `G` of `-A = G^T G` (one
  row per link and per loss), not Jacobi on `A`: a slow rate is found to the
  rounding of itself, not of the fastest rate in the system. There is no
  threshold below which a rate is called zero.
- In the step, the amounts of a group that loses nothing are made to add up
  to its mode before they are handed back, so a rounding that is the same on
  every step cannot add up over a million of them.

Consequences:

- stiffness does not matter: a valve with a millisecond time constant and an
  outgassing rate of hours coexist, at any `dt`;
- one step of an hour equals 3600 steps of a second to rounding, so advancing
  lazily to `clock.now()` on each query is exact and the answer does not
  depend on how often anything polls;
- amounts stay finite and never go below zero. A step that is not a time
  (zero, negative, infinite, NaN) is no step.

Accuracy is to the rounding of the largest pressure in a group of linked
volumes, not of each volume by itself.

No new dependency.

### 3.5 `GasNetwork`

```cpp
class GasNetwork {
 public:
  static Result<GasNetwork> make(GasTopology topology);          // Config error on a bad description
  void advance(double seconds);
  void set_valve(std::string_view name, bool open);              // rebuilds the six flows; unknown: ignored
  bool valve_open(std::string_view name) const;
  bool has_volume(std::string_view name) const;
  Result<Composition> partial_pressures(std::string_view volume) const;
  Result<double> pressure(std::string_view volume) const;        // total
  Result<void> set_partial_pressures(std::string_view volume, const Composition& mbar);
  // Adds gas (mbar L per species) to a volume: what a heated sample
  // releases. No caller yet; the entry point for laser and furnace sims.
  Result<void> inject(std::string_view volume, const Composition& mbar_litres);
  // A volume joined to nothing (a gauge off the canvas), with pumps on it.
  Result<void> add_volume(GasVolume volume, const std::vector<GasPump>& pumps = {});
  std::vector<std::pair<std::string, std::string>> valves_without_physics() const;
};
```

`make` refuses (a `Config` error naming the item) a volume or valve with no
name or a name used twice, a size that is not above zero, a pump on a volume
that is not there, and any pressure, source, loss, conductance, speed or base
that is negative or not finite. Not thread-safe and knows no clock:
`SimSystem` holds the mutex and the time.

## 4. Gas sources

### 4.1 Pipettes

Nothing special is modelled. A tank is a volume with a starting composition; a
pipette is a small volume between two valves. The script's own sequence (open
the tank side, wait, close, open the line side) loads and delivers the shot.
What follows from the equations:

- the shot is the tank pressure times the pipette volume;
- each shot leaves the tank at `V_tank / (V_tank + V_pipette)` of what it was,
  so a run of air shots declines by that factor;
- a pipette opened to the line before it has filled delivers less, and one
  whose tank valve is left open delivers the tank.

Named compositions, as ratios to Ar36:

| Name | Ar40 | Ar39 | Ar38 | Ar37 | active |
|---|---|---|---|---|---|
| `air` | 298.56 | 0 | 0.1885 | 0 | 106 times the argon sum |
| `cocktail` | 298.56 | 20 | 0.1885 | 0.5 | 0 |

Both are built in. `sim.toml` may define any number of others, and a table
called `air` or `cocktail` replaces the built-in one for the volumes that
name it (not for walls, leaks or pump bases, which are always built-in air).

### 4.2 Blank, outgassing, leaks

- Every volume of the line has an outgassing source, proportional to its
  volume: argon in the ratios of air, `outgassing` mbar L / s of it Ar40 per
  litre (default 5e-13), and active gas at `outgassing_active` (1e-10 per
  litre, no `sim.toml` key). A gauge that is not on the canvas is no part of
  the line and has no walls.
- A leak is an extra source of air on one volume, given as mbar L / s of
  Ar40.

A line valved off from its pumps therefore rises on its gauges, a blank run
measures a small atmospheric signal that grows with how long the line was
static, and a leaking stage shows as a blank that will not come down.

## 5. The spectrometer

### 5.1 The source is a volume

The canvas stage of `kind = "spectrometer"` is the source volume (the first
by name if there are several; the line warns of the others). Two terms on it
give static-mode evolution:

- ion consumption: a first-order loss on the five argon species (default
  `2e-5 /s`, 2.4 % over a twenty-minute measurement);
- memory: a constant source of `Ar40` (default 0.01 fA/s, turned into gas
  through the sensitivity), so a small signal rises and a large one falls.

There is no ion pump of the source's own in the model. A pump is an ordinary
pump on a pump stage, and the source is pumped by opening the valves that
lead to one, as the example's post-measurement script does.

### 5.2 The beam reads it

```cpp
// BeamSettings
std::function<std::vector<BeamGas>(TimePoint)> gas_at;   // empty: the fixed list

// BeamModel
void set_gas_provider(std::function<std::vector<BeamGas>(TimePoint)> provider);
std::vector<std::string> detector_names() const;
const Clock& clock() const noexcept;
```

`BeamModel::true_signal_locked` asks the provider for the instant of the
reading when one is set; a provider's `rate_per_s` is ignored. `peak_center`
looks an isotope up in the provider's list, then in the fixed one. The
provider is called with the model's mutex held.

```cpp
// SimSystem
std::optional<std::string> spectrometer_volume() const;
// Peak-top signal (fA) of each argon isotope in the source at t: its
// partial pressure times settings().source.sensitivity. An empty function
// when the line has no spectrometer volume.
std::function<std::vector<BeamGas>(TimePoint)> beam_gas();
```

The model is advanced to `t`, never past the clock's now and never back. The
function holds the system weakly: called after the `SimSystem` is gone it
answers with no gas.

The join is one function, the only place it happens:

```cpp
// libs/systems, spectrometer/bringup.hpp
Result<void> feed_beam_from_line(sim::BeamModel& beam, sim::SimSystem& line);
struct SpectrometerBringup { bool sim_beam_from_table; bool require_sim; sim::SimSystem* line_sim; };
```

It sets the provider and applies `[detectors.*]` baselines, and it checks
first and changes nothing on an error: a detector name the beam does not
have (the error names the file, the key and the detectors there are), a
baseline that is not finite, a beam and a line on different clocks. The
clock must outlive the beam; the `SimSystem` need not. Lock order is the
beam's mutex, then the line's; the line never calls a beam.

`pychron-ui` and `LabSession`'s callers pass `line_sim` to
`load_spectrometer_for_app`; `elctl exp` builds its beam by hand and calls
`feed_beam_from_line`. The join is made whenever the line has a `SimSystem`
and the spectrometer is simulated, whether `--sim` was given or the files
are simulated already. A spectrometer simulated on its own (the spectrometer
window, the spectrometer tests), or beside a line with no spectrometer
stage, has no provider and keeps the fixed argon defaults.

### 5.3 Baseline and noise

- `BeamDetector` has `baseline` (fA, or cps of dark counts on a counter,
  added to every reading wherever the magnet is) and `baseline_drift_per_h`,
  set by `BeamModel::set_baseline`. Off-peak the detector reads baseline plus
  noise, so baseline correction has something to correct. A baseline never
  overloads a counter.
- Noise is keyed (`keyed_noise.hpp`): a draw is a function of
  `(seed, key, tick)`, the key being the detector's name or the gauge's
  volume and the tick the nanoseconds since the model was built. A reading
  then depends on which detector and when, not on which thread asked first,
  and a simulated run gives the same numbers every time (virtual clock spec
  §3.6). `keyed_gauss` serves Faradays and gauges. `keyed_poisson` serves
  counters: Knuth's multiplication below a mean of 30, a rounded normal from
  30 up. Both are fixed integer arithmetic up to the maths library's `exp`,
  `log`, `sqrt` and `cos`, so counts are the same on every platform where
  those agree to the last place. One key serves one of the two: they draw
  from the same uniforms.
- `BeamModel::counter_yield(det)` is the fraction of the ions reaching a
  counter that it counts (the multiplier's plateau at its voltage; 1 for a
  Faraday): what a test or a reduction divides a counted ratio by.

## 6. Configuration

### 6.1 From the canvas

`topology_of()` in `extraction_line.cpp` turns the canvas network into a
`SimTopology` of volumes with roles, valves and edges.

```cpp
enum class SimRole { Plain, Pump, Getter, Tank, Pipette, Spectrometer, Gauge, Pipe };
struct SimVolume { std::string name; double cc = 0.0; SimRole role = SimRole::Plain; };
```

| On the canvas | Role | Becomes | Size with none given |
|---|---|---|---|
| stage of kind `pump` | `Pump` | a volume with a `GasPump` (50 L/s, 1e-9 mbar) | 50 cc |
| stage of kind `getter` | `Getter` | a volume with a `GasPump{nobles = false}` (1 L/s, base 0) | 50 cc |
| stage of kind `tank` | `Tank` | a volume starting with air at 3e-5 mbar of Ar40 | 50 cc |
| stage of kind `spectrometer` | `Spectrometer` | the source: consumption and memory; the beam reads it | 50 cc |
| stage of kind `pipette`, or a `[[pipette]]` | `Pipette` | a small volume | 0.1 cc |
| `[[gauge]]` | `Gauge` | a small volume | 1 cc |
| any other stage | `Plain` | a volume | 50 cc |
| two valves joined directly | `Pipe` | a volume `"<a>~<b>"` between them | 1 cc |

A stage's `volume` (cc) is its size. Every volume but a tank starts at the
default pressure, as air.

Pipes. A canvas draws pipe straight from one valve to the next, and a valve
needs a volume on each side. So each pair of valves joined directly gets a
`Pipe` volume between them, named for the two with `~` between, in byte
order. The exception is a pair that a tee joins to each other and to one
volume: both are on that volume already and no pipe is made. A canvas valve
with `~` in its name is refused when the line builds its sim (`Config`), so
two pairs cannot make one name. A tee of three valves is not modelled: each
of the three then has three neighbours and no physics.

The valves that carry no gas are logged once, in one line at info, with why
for each. A second spectrometer stage is a warning.

A gauge of the line that is not on the canvas becomes an isolated volume when
its controller's sim hook is made, with the size, gas, leak and pump the
settings give its name.

### 6.2 `sim.toml`

Optional. Named by `[sim] file = "..."` in the line's file (relative to it; a
file named and not found is an error), else a `sim.toml` beside it. It is
read only when the line is loaded with its canvas, because its names are the
canvas's; a line loaded alone (`elctl laser`) says at info that the named
file is not read. `load_sim_settings(file, topology, base)` in
`libs/sim/src/sim_config.cpp` reads it over a base:

```
built-in defaults  <  ExtractionLine::Options::sim (the caller's)  <  keys present in the file
```

```toml
[defaults]
pressure = 1e-8            # mbar, as air: every volume not listed (a tank starts full)
volume_cc = 50             # a stage with no size
pipe_cc = 1                # the pipe between two valves joined directly
gauge_cc = 1               # a gauge
valve_conductance = 0.1    # L/s for Ar40; tau = V1 V2 / ((V1 + V2) C)
outgassing = 5e-13         # mbar L / s of Ar40 per litre of volume
noise = 0.01               # relative 1-sigma on gauges
seed = 0x5eed              # of the gauge noise

[compositions.cocktail]    # ratios to Ar36; a species not given is 0
Ar36 = 1
Ar37 = 0.5
Ar38 = 0.1885
Ar39 = 20
Ar40 = 298.56
active = 0

[volumes.air_tank]
composition = "air"        # air, cocktail or a [compositions.*] name
argon40 = 3e-5             # mbar of Ar40; the rest follows the composition
# pressure = ...           # mbar in all, instead of argon40
leak = 0                   # mbar L / s of Ar40, as air
volume_cc = 50             # over the canvas's `volume`

[valves.B]
conductance = 0.1          # L/s for Ar40

[pumps.turbo]
speed = 50                 # L/s
base = 1e-9                # mbar

[spectrometer]
sensitivity = 1e12         # fA per mbar of an isotope in the source
consumption = 2e-5         # 1/s
memory_fA_per_s = 0.01     # as Ar40

[detectors.H1]
baseline = 0               # fA, or cps on a counter
baseline_drift_per_h = 0
```

These are the defaults. Names are canvas names (a stage, pipette, gauge or
pipe; a valve; a volume for a pump), and the gauges the line has off the
canvas. Detector names are the spectrometer's and are checked by
`feed_beam_from_line`, which is why `SimSettings` carries the file's name.

Everything is checked and every problem is reported at once, as
`file:line:key: message` (`Config`):

- an unknown table, key, species, volume, valve, pump volume or composition;
  the message lists the known ones, or counts them above twelve;
- a number that is not finite or is outside its range, told the range and
  the unit: pressures 0 to 1e4 mbar; sizes 1e-6 to 1e9 cc; conductances and
  pump speeds 0 to 1e9 L/s; outgassing and leaks 0 to 1e3 mbar L / s;
  consumption 0 to 1e6 /s; sensitivity above 0 to 1e30 fA/mbar; noise 0 to
  10; ratios 0 to 1e9; memory 0 to 1e30 fA/s and no more than 1e3 mbar/s
  through the sensitivity; baselines within 1e30 of zero; seed a whole
  number of zero or more;
- `argon40` and `pressure` both on one volume; a composition with no Ar40
  given an `argon40`; a species that comes out above 1e4 mbar.

`SimSettings` keeps `initial_pressures`, `pumps`, `noise` and `seed` for the
tests that fill it in code, and gained the rest: `default_volume_cc`,
`valve_conductance`, `outgassing`, `outgassing_active`, `compositions`,
`conductances`, `leaks`, `getters`, `sizes`, `pump_speeds`, `tank_argon40`,
`pipette_cc`, `gauge_cc`, `pipe_cc`, `pump_speed`, `pump_base`,
`getter_speed`, `source`, `detectors`, `named`, `file`. Those with no
`sim.toml` key (`outgassing_active`, `getters`, `tank_argon40`,
`pipette_cc`, `pump_speed`, `pump_base`, `getter_speed`) are set in code
only; the file reaches a tank's gas, a pipette's size and a pump through
`[volumes.*]` and `[pumps.*]`.

`configs/examples/sim.toml` documents every key with its default, each
commented out, so that as documentation it overrides nothing, and then sets
the five numbers the example lab is tuned by: `[defaults] pressure = 1e-10`
and `outgassing = 1.5e-13`, `[volumes.air_tank] argon40 = 9.5e-5`,
`[valves.B] conductance = 0.02`, `[pumps.turbo] base = 1e-10`. With them one
air shot reads 9.5e4 fA of `Ar40` and a blank about 10 fA.

## 7. `SimSystem` after the change

- Holds a `GasNetwork`, the clock, the mutex and the device sims.
- `set_valve`, `pressure`, `gauge_reading`, `has_volume`, `hook_for*`,
  `chromium`, `heater`: unchanged signatures. `pressure` is the total.
- `set_pressure(volume, value)` scales the volume's composition to that total
  (air if it was empty).
- New: `partial_pressures(volume)`, `set_composition(volume, mbar)`,
  `inject(volume, mbar_litres)`, `spectrometer_volume()`, `beam_gas()`,
  `clock()`, `settings()`, `valves_without_physics()`, `build_error()`.
- A topology and settings that do not describe a network (a negative
  pressure, a size that is not a number) leave the line empty with the
  refusal in `build_error()`, which `ExtractionLine` returns once the hooks
  are made. The constructor's signature did not change.
- It keeps its own map of what every valve name was last told, the
  network's valves and the names it does not have (switches, unmodelled
  valves) alike.

### 7.1 A change in behaviour

Equilibration is no longer instant. With the defaults a valve between two
50 cc volumes equilibrates with a time constant of 0.25 s. A test that opens
a valve and reads a pressure advances the clock first (a few time constants)
or sets a large conductance where the point of the test is elsewhere. And
since nothing moves in zero elapsed time, a test that asserts something did
not move advances the clock before it looks; where it asserts an exact hold
it zeroes outgassing, which would otherwise raise every volume by
1e-10 mbar/s.

Pump-down keeps its form: a pumped volume decays exponentially to the pump's
base pressure. `SimPump{base, tau}` maps to `speed = V / tau`, `V` being the
pump's own volume; `pump_speeds` gives a speed outright.

## 8. Tests

`tests/sim/test_linear_flow.cpp` (`Gas`, `LinearFlow`): air and cocktail
ratios; two volumes tend to the volume-weighted mean with the right time
constant; amounts are conserved with no loss or source; one step equals many;
a pumped volume decays to its base; a closed region with a source grows
linearly; conductances spanning many orders and long steps stay finite and
non-negative; a slow loss beside a fast valve keeps its rate; a lossless
group beside a lossy one; many tiny steps do not drift; a step that is not a
time does nothing; bad terms are refused.

`tests/sim/test_gas_network.cpp`: valves start closed and isolate; an open
valve equilibrates each species; lighter gas arrives first; a pipette
delivers tank pressure times its volume; ten shots decline geometrically (the
line emptied after each); a tank valve left open empties the tank; pump,
getter, outgassing, first-order loss and memory; merged volumes; `inject`
adds exactly the amount; isolated and pumped added volumes; valves without
physics are tracked and explained; bad topologies are refused; each species
is conserved through valve toggles; one step equals many on a pumped
network; many toggles stay physical.

`tests/sim/test_keyed_noise.cpp`: the same key gives the same draw, another
key or tick another; unit variance; Poisson mean and variance on both
methods and no step where they switch; pinned counts.

`tests/sim/test_sim_config.cpp`: an empty file gives the defaults; every key
is read and stored; a key in the file goes before the base and the rest
stands; a volume's gas from its composition, `argon40` or `pressure`; unknown
names, keys and compositions, negative, non-finite and out-of-range numbers
are refused, every problem with its line and the known names; the example
file's commented keys are the code's defaults and its active lines are
exactly the five.

`tests/sim/test_beam_model.cpp`: with a provider the signal follows it and
without one nothing changes; baseline is read off-peak and adds on the peak;
the same seed, detector and time give the same reading whatever the call
order; `set_gas` restarts neither the noise nor the drift; a counter's
baseline is dark counts; the counter's yield is the plateau at its voltage.

`tests/sim/test_sim_system.cpp`: existing cases, updated per 7.1; roles;
gauge noise keyed by volume and time; the beam's gas; a refused network.

`tests/systems/test_extraction_line.cpp`: roles from canvas kinds; the
`sim.toml` beside the line, the one named, none without a canvas; pipes
between valves joined directly and their sizes; the tee rule; the one info
line; a valve named like a pipe is refused; the example lab with and without
its `sim.toml`; the NMGRL canvas.

`tests/systems/test_spectrometer_bringup.cpp`: a line feeds the beam; without
one the beam is as before; a beam read after its line is gone reads no gas;
baselines reach the detectors, also with no spectrometer volume; an unknown
detector, another clock and a refused feed change nothing.

`tests/integration/test_lab_sim.cpp` (`LabSim`), on a `VirtualClock` at
unlimited speed with a scratch copy of the example lab, its scripts, plan
and queue; expected values are worked out from the settings the line loaded:

- an air run's `Ar40` rises after the inlet opens with the inlet's time
  constant, falls during the static measurement at the consumption slope
  less what walls and memory give, and returns to baseline after the
  post-measurement script pumps the source. Before the inlet opens the
  reading is baseline plus the pumped source's own level, not baseline;
- 40/36 of three air runs pooled, Ar36 divided by the counter's yield, is
  within 2 % of 298.56, and as counted within 2 % of 298.56 over the yield.
  A check with no reading in it holds the 2 % to five standard errors of the
  design;
- a blank is above zero, under 1 % of an air run, and what the walls gave;
- three air runs decline by the tank's depletion factor within counting
  error;
- a run whose extraction script omits the pipette measures a blank;
- a tank valve left open drains the tank;
- two labs built afresh give identical records;
- the five-run queue takes no real time.

## 9. Order of work, as done

1. Species and `LinearFlow` (`f96c3ea`, `45f8b22`).
2. `GasNetwork` (`030a42e`, `33d3337`).
3. `SimSystem` on `GasNetwork`, keyed gauge noise; existing tests updated
   per 7.1 (`2d4dc18`, `d8e2d3b`, `c00f816`).
4. Roles from the canvas and `sim.toml`; pipes between valves
   (`3168d9b`, `710f6cd`, `c331ebe`).
5. `BeamModel`: provider, baseline, keyed noise (`0eaf234`, `55bcca9`).
6. The join: `feed_beam_from_line`, `elctl exp`, `pychron-ui`
   (`329ab03`, `fe5fa39`, `03af8ff`).
7. The example lab's air queue and `test_lab_sim.cpp`; keyed Poisson
   (`a544105`, `9bb6624`, `63c3782`, `c1279c0`).
8. `docs/simulator.md`, this document, small items from review (`fad09c2`).

## 10. Not yet

- **Gas from a laser or a furnace.** `SimSystem::inject` is the entry point
  and has no caller: firing the simulated laser or heating the simulated
  furnace releases nothing. The example's stock scripts admit a pipette of
  air in its place.
- **Calibrated amounts.** No irradiated sample, no J, no age to recover.
- **A getter on the example canvas.** The model has the role; the example
  draws none, so an air shot's active gas stays in the source until it is
  pumped.
- **A tee of three valves**, and any valve that does not stand between
  exactly two volumes: tracked, reported, no gas.
- **The gas model behind `elctl`'s hardware commands.** `elctl open`,
  `read`, `scan` and the `sim` session use `elctl`'s own device sims, not
  `ExtractionLine`: gauges there read a fixed number.
- **A seed for the detectors from `sim.toml`.** `[defaults] seed` is the
  gauges'; the beam's is `BeamSettings::seed`, which the applications leave
  at its default.
- **A check of a queue's plan against the plan's `analysis_types`.**
- Fault injection and a simulator control panel, as in section 1.

## 11. Decisions made while building

Each is a departure from the first draft of this document or from the plan.

- **The solver.** The draft said Jacobi eigenvalue iteration on the
  symmetrized matrix with the zero eigenvalue handled as linear growth. A
  threshold for "zero" failed the stiff conservation test (1.4e-7 against
  1e-9) and would have zeroed slow rates beside fast valves. Built instead:
  conserved modes removed exactly, one-sided Jacobi on a factor of the
  matrix, no threshold, and a mend of rounding in each step (drift 2e-14
  over 20 000 toggles). Checked against a 90-digit reference.
- **Equilibration takes time, and tests must let it.** Section 7.1. Two
  "nothing moved" assertions had become vacuous because nothing moves in
  zero elapsed time; the rule that such a test advances the clock, and may
  zero outgassing for an exact hold, came from that. A time-constant test
  asserts on the Ar40 partial pressure, since air is mostly active gas with
  a shorter time constant.
- **Implicit pipes and the tee rule.** Section 6.1. On the NMGRL canvas 17
  of the valves had no physics because the drawing joins valve to valve;
  with pipes and the tee rule seven remain, each drawn with one side or
  none. Cost if wrong: a 1 cc dead volume per join, sizable in `sim.toml`.
- **Gauges and pipes are 1 cc**, not the 50 cc default: a gauge head and a
  length of pipe are small appendages.
- **`sim.toml` is not read without the canvas**, since its names are the
  canvas's and `elctl laser` loads lines alone.
- **The example file overrides nothing as documentation.** Its keys are
  commented out, so precedence is defaults, the caller's options, the keys
  present; only the five tuned values are in force.
- **Defaults were retuned by arithmetic for plausible signals**: outgassing
  5e-13 mbar L / s of Ar40 per litre with active gas at 1e-10 (the draft:
  1e-13), a tank at 3e-5 mbar of Ar40 (2e-7), default pressure 1e-8 (1e-9),
  and the example's own five numbers on top.
- **A lab simulated by its config is joined without `--sim`.** The first
  wiring gated the join on the flag and left two simulators unjoined when
  the files were simulated already.
- **The join checks the clocks and changes nothing on failure.** A beam and a
  line on different clocks would put a reading's instant outside the line's
  time.
- **The counter's yield, and the ratio as counted.** The example's CDD at
  1450 V counts 98.47 % of what reaches it, so air's 298.56 reads 303.2 as
  the two detectors report it, as in a lab before intercalibration. The
  spectrometer config was left alone; `BeamModel::counter_yield` exposes the
  factor and the test asserts both forms.
- **Keyed Poisson.** Counter noise first came from
  `std::poisson_distribution`, which differs between libc++, libstdc++ and
  MSVC, so a statistical test that passed here could fail on another
  compiler for good. `keyed_poisson` is written out and the same everywhere.
- **The ratio test pools three runs** and holds the design, not the draw, to
  the margin: one run's Ar36 is Poisson-limited at 0.64 %, and 2 % was three
  standard errors of it.
- **The stock scripts admit a pipette of air** for runs that are not blanks,
  as the stand-in for sample gas. With the source pumped between runs the
  stock unknowns otherwise read a blank and trip their own no-gas
  termination.
- **The reading before the inlet is baseline plus the pumped source's
  level**, not baseline: with a blank of 10 fA the two cannot both be true.
  After the pump-out the reading is within noise of baseline, which is why
  the example's turbo has a base of 1e-10 mbar.
- **Determinism is asserted on two cycles a run**, not five: ten full runs
  do not fit a test's time under ThreadSanitizer.
- **`blank_air` was added to the example plan's `analysis_types`**, so the
  Measurement dock offers the plan for the air queue's blanks.
