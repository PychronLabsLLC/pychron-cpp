# Lab simulator: gas from the line to the detector

Date: 2026-10-06
Status: Draft
Owner: Jake Ross
Builds on: `2026-09-29-instrument-control-design.md` §7 (`SimSystem`),
`2026-09-29-spectrometer-control-design.md` §8.1 (`BeamModel`),
`2026-10-06-virtual-clock-design.md` (simulated time).

## 1. Intent

Two simulators exist and do not know about each other.

- `SimSystem` models the extraction line: volumes and valves as a graph, one
  pressure per volume, instant equilibration across open valves, exponential
  pump-down, gauge noise. Every valve, gauge, heater and laser driver of a
  `kind = "sim"` config talks to it.
- `BeamModel` models the ion beam: peak shapes, detector noise, dead time. Its
  gas is a fixed list of abundances with one exponential rate each, started
  when the model is built.

So a simulated run measures the same signal whatever the extraction script
did. Opening the inlet changes nothing at the detector; an air shot and a
blank are the same analysis; a script that never opens the pipette looks fine.

This spec joins them. The line carries gas by species, the spectrometer's
source is one of the line's volumes, and the beam reads that volume. The goal
is that pychron run with `--sim` behaves as it would on a lab: the signals
have the right shape at the right time for what the valves did.

For: development and UI work without hardware, demonstrations and training,
and integration tests of the executor, scripts and conditionals that run in
milliseconds under the virtual clock.

In the first version: air and cocktail pipettes; line blank, outgassing and
leaks.

Not in scope: gas released by a laser or furnace (the model has the entry
point, section 3.5); recovering a known age from a simulated sample (amounts
are plausible, not calibrated); fault injection beyond what the device sims
already do; a simulator control panel.

## 2. Shape

```
canvas + sim.toml ──> GasNetwork  (pure physics, no devices)
                          ^   ^
        valve events,     |   |   partial pressures of the
        gauge reads       |   |   spectrometer volume
                          |   |
                     SimSystem ──────> BeamModel
                     (device hooks)    (peaks, detectors, noise)
```

- `GasNetwork` (new) owns the state and the equations. It depends on
  `pychron::core` only and is tested on its own.
- `SimSystem` keeps its device hooks and its public API and delegates the
  physics to a `GasNetwork`.
- `BeamModel` takes an optional gas provider. Without one it behaves as
  today.

## 3. `GasNetwork`

`libs/sim/include/pychron/sim/gas_network.hpp`, `libs/sim/src/gas_network.cpp`.

### 3.1 State

A partial pressure (mbar) per species per volume. Species are fixed:

| Species | Mass | Note |
|---|---|---|
| `Ar36` .. `Ar40` | 35.968 .. 39.962 | what the spectrometer measures |
| `active` | 28 | everything a getter removes (N2, O2, H2O, CO2), as one bulk gas |

A volume's total pressure is the sum over species. `active` is there so that
gauges read sensibly: air is about 1 % argon, and a line that has taken an air
shot and not yet gettered it must show it.

### 3.2 Elements

Built from a description (section 6 says where it comes from):

```cpp
struct GasVolume   { std::string name; double litres; Composition initial;
                     Composition source_per_s;          // outgassing + leak, mbar L / s
                     SpeciesRates loss_per_s; };        // first order, 1 / s
struct GasValve    { std::string name; double conductance; };   // L / s for Ar40
struct GasPump     { std::string volume; double speed; double base;      // L / s, mbar
                     bool nobles = true; bool active = true; };
struct GasTopology { std::vector<GasVolume> volumes; std::vector<GasValve> valves;
                     std::vector<GasPump> pumps;
                     std::vector<std::pair<std::string, std::string>> edges; };
```

- A valve conducts only while open. Its conductance for species of mass M is
  `conductance * sqrt(40 / M)` (molecular flow), so lighter argon arrives
  slightly sooner and an inlet fractionates a little until it equilibrates.
- Volumes joined by an edge with no valve between them are merged into one
  volume at build time.
- A pump is a sink on its volume: `dp/dt = -(speed / V) * (p - base_share)`,
  with the base pressure shared among species in proportion to an air
  composition. `nobles = false` makes a getter: it pumps `active` only.
- `source_per_s` is a constant inflow; `loss_per_s` a first-order loss. They
  are how outgassing, leaks, ion consumption and source memory are expressed
  (sections 4 and 5).

### 3.3 Equations

For each species, with `n_i = p_i V_i`:

```
dn_i/dt = sum_j C_ij (n_j / V_j - n_i / V_i)      open valves between i and j
          - (S_i / V_i) (n_i - base_i V_i)        pumps on i
          - L_i n_i                               first-order loss in i
          + q_i                                   constant source in i
```

That is `dn/dt = K n + s` with `K` and `s` constant between valve events.
Species do not interact, so there is one small system per species (as many
unknowns as volumes, tens at most).

### 3.4 Solution

`K` is similar to a symmetric matrix (scale row and column i by `sqrt(V_i)`).
When the topology or a valve changes, each species' system is decomposed once
(Jacobi eigenvalue iteration; no new dependency). Advancing by any `dt` is
then exact:

```
n(t + dt) = n_inf + U exp(Lambda dt) U^T (n(t) - n_inf)
```

with the zero eigenvalue of a closed, lossless, sourceless region handled as
linear growth (`+ s dt` along that mode). Consequences:

- stiffness does not matter: a valve with a 0.1 s time constant and an
  outgassing rate of hours coexist;
- one step of an hour equals 3600 steps of a second to rounding, so advancing
  lazily to `clock.now()` on each query, as `SimSystem` does now, stays exact
  and the answer does not depend on how often anything polls.

### 3.5 API

```cpp
class GasNetwork {
 public:
  explicit GasNetwork(GasTopology topology);
  void advance(double seconds);
  void set_valve(std::string_view name, bool open);   // re-decomposes
  bool valve_open(std::string_view name) const;
  Result<Composition> partial_pressures(std::string_view volume) const;
  Result<double> pressure(std::string_view volume) const;            // total
  Result<void> set_partial_pressures(std::string_view volume, const Composition&);
  // Adds gas (mbar L per species) to a volume: what a heated sample
  // releases. Unused in v1; the entry point for laser and furnace sims.
  Result<void> inject(std::string_view volume, const Composition& amount);
  bool has_volume(std::string_view name) const;
  void add_volume(GasVolume volume);                  // isolated; for gauges off the canvas
};
```

Not thread-safe and knows no clock: `SimSystem` holds the mutex and the time.

## 4. Gas sources in v1

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

Named compositions, usable wherever a composition is configured:

| Name | 40/36 | 38/36 | 39/36 | 37/36 |
|---|---|---|---|---|
| `air` | 298.56 | 0.1885 | 0 | 0 |
| `cocktail` | 298.56 | 0.1885 | 20 | 0.5 |

`air` also carries `active` at 106 times its argon. `cocktail` values are
defaults; `sim.toml` may define any number of named compositions.

### 4.2 Blank, outgassing, leaks

- Every volume has a default outgassing source, proportional to its volume,
  mostly `active` with a trace of atmospheric argon.
- A leak is an extra source of `air` composition on one volume, set in
  `sim.toml`.

A line valved off from its pumps therefore rises on its gauges, a blank run
measures a small atmospheric signal that grows with how long the line was
static, and a leaking stage shows as a blank that will not come down.

## 5. The spectrometer

### 5.1 The source is a volume

The canvas stage of `kind = "spectrometer"` is the source volume. Two terms on
it give static-mode evolution:

- ion consumption: a first-order loss on the argon species (default
  `2e-5 /s`, a few percent over a twenty-minute measurement);
- memory: a constant source, mostly `Ar40` (default equivalent to about
  0.01 fA/s), so a small signal rises and a large one falls, as on an instrument.

The ion pump is an ordinary pump behind its valve. Opening it after the
measurement pumps the source down to its base pressure.

### 5.2 The beam reads it

```cpp
// BeamSettings
std::function<std::vector<BeamGas>(TimePoint)> gas_at;   // empty: `gas` as today
```

`BeamModel::true_signal_locked` asks `gas_at(t)` when it is set. `SimSystem`
provides it:

```cpp
// Peak-top signal (fA) of each argon isotope in `volume` at time t:
// partial pressure times `sensitivity` (fA / mbar).
std::function<std::vector<BeamGas>(TimePoint)> beam_gas(std::string volume, double sensitivity);
```

`ExtractionLine` exposes the line's sim as it does now. Where the app builds
both a simulated line and a simulated spectrometer (`elctl exp`, `pychron-ui`,
`LabSession`), it sets `gas_at` on the beam settings before the model is
registered. A spectrometer simulated on its own (the spectrometer window, the
spectrometer tests) has no provider and keeps the fixed argon defaults.

`BeamGas::rate_per_s` stays for that fixed case and is ignored when `gas_at`
is set.

### 5.3 Baseline and noise

- `BeamDetector` gains `baseline` (fA or cps, added to every reading whatever
  the magnet position) and `baseline_drift_per_h`. Off-peak the detector reads
  baseline plus noise instead of zero plus noise, so baseline correction has
  something to correct.
- Noise is drawn from a generator seeded by `hash(seed, detector, t)` for
  each reading instead of from one shared generator. A reading's value then
  depends on which detector and when, not on which thread asked first, and a
  simulated run gives the same numbers every time (virtual clock spec §3.6).
  Gauge noise in `SimSystem` is drawn the same way, keyed by volume and time.

## 6. Configuration

### 6.1 From the canvas

`topology_of()` in `extraction_line.cpp` already turns the canvas network into
volumes, valves and edges. It additionally maps stage kinds:

| Canvas `kind` | Becomes |
|---|---|
| `pump` | a volume with a `GasPump` (speed and base by default) |
| `getter` | a volume with a `GasPump{nobles = false}` |
| `tank` | a volume starting with `air` at the default tank pressure |
| `pipette` | a volume, initially at line pressure |
| `spectrometer` | the source volume: consumption and memory terms; the beam reads it |
| anything else | a volume with default outgassing |

A stage's `volume` (cc) is its size; stages without one get the default.

### 6.2 `sim.toml`

Optional, beside the lab's `extraction_line.toml`; named by
`[sim] file = "sim.toml"` there or found by that name. Without it the defaults
below give a working lab.

```toml
[defaults]
pressure = 1e-9            # mbar, every volume not listed
volume_cc = 50
valve_conductance = 0.1    # L/s for Ar40; tau = V / C
outgassing = 1e-13         # mbar L / s per litre
noise = 0.01               # relative 1-sigma on gauges
seed = 0x5eed

[compositions.cocktail]    # ratios to Ar36
Ar40 = 298.56
Ar39 = 20
Ar38 = 0.1885
Ar37 = 0.5
active = 0

[volumes.air_tank]
composition = "air"
argon40 = 2e-7             # mbar of Ar40; the rest follows the composition

[volumes.cocktail_tank]
composition = "cocktail"
argon40 = 2e-7

[volumes.bone]
leak = 0                   # mbar L / s of air

[valves.inlet]
conductance = 0.05

[pumps.turbo]
speed = 50                 # L/s
base = 1e-9

[spectrometer]
sensitivity = 1e12         # fA per mbar of an isotope in the source
consumption = 2e-5         # 1/s
memory_fA_per_s = 0.01     # as Ar40

[detectors.H1]
baseline = 50
baseline_drift_per_h = 2
```

Names are canvas names. An unknown name is a config error naming the file and
the key, as elsewhere. `SimSettings` keeps `initial_pressures`, `pumps`,
`noise` and `seed` for the tests that fill it in code; `sim.toml` is parsed
into the same structure, extended.

Defaults are chosen so that one air shot from the example canvas gives an
`Ar40` signal of a few times 10^4 fA and a blank a few tens. The numbers in the
example above show the keys, not the tuned defaults.

## 7. `SimSystem` after the change

- Holds a `GasNetwork`, the clock, the mutex and the device sims.
- `set_valve`, `pressure`, `gauge_reading`, `has_volume`, `hook_for*`,
  `chromium`, `heater`: unchanged signatures. `pressure` is the total.
- `set_pressure(volume, value)` scales the volume's composition to that total
  (air composition if it was empty).
- New: `partial_pressures(volume)`, `set_composition(volume, composition)`,
  `inject(volume, amount)`, `beam_gas(volume, sensitivity)`.

### 7.1 A change in behaviour

Equilibration is no longer instant. With the defaults a valve between two
50 cc volumes equilibrates with a time constant near half a second. Tests in
`tests/sim/test_sim_system.cpp` and `tests/integration` that open a valve and
read the mean pressure at once advance the clock first (a few time constants)
or set a large conductance where the point of the test is elsewhere.

Pump-down keeps its form: a pumped volume decays exponentially to the pump's
base pressure. `SimPump{base, tau}` maps to `speed = V / tau`.

## 8. Tests

`tests/sim/test_gas_network.cpp`:

- closed network, no sources or losses: `sum(p V)` per species is conserved
  through any sequence of valve events;
- two volumes, valve opened: both tend to the volume-weighted mean, with time
  constant `V1 V2 / ((V1 + V2) C)`;
- pumped volume: `base + (p0 - base) exp(-t S / V)`;
- exactness: one step of 3600 s equals 3600 steps of 1 s to 1e-9 relative;
- stiffness: conductances spanning 1e6 give finite, non-negative pressures;
- pipette: after the load and deliver sequence the line holds
  `p_tank V_pipette / V_line_total`; ten shots decline geometrically by
  `V_tank / (V_tank + V_pipette)`;
- fractionation: just after an inlet opens the downstream 36/40 is above the
  upstream ratio, and returns to it at equilibrium;
- static rise: an isolated volume's pressure grows linearly at
  `outgassing / V`;
- getter: removes `active`, leaves argon;
- merged volumes: an edge with no valve behaves as one volume;
- `inject` adds exactly the amount given.

`tests/sim/test_beam_model.cpp`: with `gas_at`, the signal follows the
provider; baseline is read off-peak; the same seed, detector and time give
the same reading whatever the call order.

`tests/sim/test_sim_system.cpp`: existing cases, updated per 7.1; `sim.toml`
parsing, defaults from canvas kinds, unknown names refused.

`tests/integration/test_lab_sim.cpp`, on a `VirtualClock` at unlimited speed
with the example lab:

- an air run through the executor with the stock scripts and plan: the `Ar40`
  signal rises after the inlet opens with the configured time constant,
  evolves with the consumption slope during the static measurement, and
  returns to baseline after the ion pump opens;
- the reduced 40/36 of that run is within 2 % of 298.56;
- a blank run's `Ar40` is under 1 % of the air run's;
- three air runs in a row decline by the tank's depletion factor within
  counting error;
- a queue whose extraction script omits the pipette measures a blank;
- two runs of the same queue give identical records.

## 9. Order of work

1. `GasNetwork` and its tests (no other code changes).
2. `SimSystem` on `GasNetwork`; existing tests updated per 7.1.
3. Canvas kinds and `sim.toml`.
4. `BeamModel`: `gas_at`, baseline, keyed noise.
5. App wiring (`elctl exp`, `pychron-ui`, `LabSession`) and
   `test_lab_sim.cpp`. This step needs the virtual clock.
6. `docs/simulator.md`: what is modelled, `sim.toml`, how to run a simulated
   lab.
