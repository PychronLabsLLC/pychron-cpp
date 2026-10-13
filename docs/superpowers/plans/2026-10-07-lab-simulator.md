# Lab Simulator Implementation Plan

> Executed 2026-10-07 on `feat/lab-simulator`. The plan grew while it ran; what was built is in the spec, `docs/superpowers/specs/2026-10-06-lab-simulator-design.md` (section 11 lists the departures), and the user guide is `docs/simulator.md`.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A simulated lab in which gas moves by species from tanks and leaks through the valves into the spectrometer's source, so the measured signal follows what the extraction script did.

**Architecture:** A new pure-math `GasNetwork` (partial pressure per species per volume, valves as conductances, pumps, sources and losses, solved exactly between valve events) replaces the scalar physics inside `SimSystem`, which keeps its device hooks and API. `BeamModel` reads the spectrometer volume through an optional gas provider. Roles come from the canvas's stage kinds; an optional `sim.toml` overrides the defaults.

**Tech Stack:** C++20, toml++ (already a dependency of `pychron::core`), GoogleTest, `VirtualClock` test helpers in `tests/support/virtual_time.hpp`.

**Spec:** `docs/superpowers/specs/2026-10-06-lab-simulator-design.md`. Read it first; section numbers below refer to it. Time rules: `AGENTS.md` "Time" and `docs/superpowers/specs/2026-10-06-virtual-clock-design.md`.

## Global Constraints

- Branch `feat/lab-simulator`, cut from `develop` after the virtual clock landed.
- Conventional Commits with the component as scope (`feat(sim): ...`, `feat(systems): ...`, `test(integration): ...`). End each message with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Build and test: `cmake --build build/dev -j` then `ctest --test-dir build/dev --output-on-failure -R <regex>`; UI tree `build/dev-ui` with `QT_QPA_PLATFORM=offscreen`. CI does not run on work branches: run the task's tests before each commit and both full suites before the last commit of Tasks 3, 6 and 8.
- Never skip or disable a failing test. A failure on one compiler only is a real bug.
- `libs/sim` depends on `pychron::devices`, `pychron::transport`, `pychron::core` only. `GasNetwork` and `linear_flow` include nothing outside the standard library and `pychron/core/error.hpp`. No new third-party dependency (no Eigen). Qt does not appear in `libs/`.
- `SimSystem`'s existing public signatures do not change. A spectrometer simulated with no line behaves exactly as today.
- Time (AGENTS.md "Time"): simulated tests use `VirtualTimeTest`, the test thread is a `Clock::Participant`, time moves with `clock.sleep_for`, no real sleeps, real-time bounds are upper bounds of 5 s or more. Code that waits follows the participant, notify and `ClockMutex` rules; this plan adds no thread.
- Units inside `GasNetwork`: pressure mbar, volume litres, amount mbar·L, time seconds, conductance and pump speed L/s. The canvas gives cc: divide by 1000 at the boundary, once.
- Every number a user can set in `sim.toml` is validated: an unknown key or name, a negative or non-finite value, is a `Config` error naming the file and the key.
- Results never contain NaN, infinity or a negative pressure, whatever the valve sequence or the step length.
- Each header a file uses is included by that file.

## Review Focus

1. A network with no pump, or a closed region with a source and no loss: pressure rises linearly for ever (the zero mode), finite at any step. (Task 1 and 2 tests.)
2. A step of a week after a step of a microsecond, and conductances spanning 1e9: finite, non-negative, mass-conserving. (Task 1 test.)
3. `sim.toml` naming a volume, valve or pump that the canvas does not have; a canvas with no `spectrometer` stage; a canvas with two: an error that names the file and key in the first case, no provider and no crash in the second, the first by name in the third with a warning. (Task 4 and 6 tests.)
4. A valve the drivers know but the canvas does not (a switch, an unmodelled valve): tracked, no physics, no error, as today. (Task 3 test.)
5. An extraction script that leaves the pipette's tank valve open while the line valve opens: the tank empties into the line and the next shots are small; nothing crashes. (Task 7 test.)

## File Structure

| File | Responsibility |
|---|---|
| `libs/sim/include/pychron/sim/gas.hpp` (new) | `Species`, `Composition`, masses, named compositions, arithmetic helpers |
| `libs/sim/include/pychron/sim/linear_flow.hpp`, `src/linear_flow.cpp` (new) | exact solution of `dn/dt = K n + s` for one species: symmetric eigen-decomposition and `advance` |
| `libs/sim/include/pychron/sim/gas_network.hpp`, `src/gas_network.cpp` (new) | topology, valves, pumps, sources, losses; builds one `LinearFlow` per species per valve event |
| `libs/sim/include/pychron/sim/sim_config.hpp`, `src/sim_config.cpp` (new) | `sim.toml` to `SimSettings`; defaults; validation |
| `libs/sim/include/pychron/sim/keyed_noise.hpp` (new) | noise that depends on (seed, name, time), not on call order |
| `libs/sim/.../sim_system.hpp/.cpp` | device hooks; delegates physics to `GasNetwork` |
| `libs/sim/.../spectrometer/beam_model.hpp/.cpp` | gas provider, baseline, keyed noise |
| `libs/systems/src/extraction_line.cpp` | canvas kinds to `GasTopology`; finds and loads `sim.toml` |
| `libs/systems/src/spectrometer/bringup.cpp`, `apps/elctl/src/exp.cpp`, `apps/pychron-ui/src/main.cpp`, `libs/experiment/src/lab/session.cpp` | hand the line's provider to the beam |
| `configs/examples/sim.toml`, `configs/examples/scripts/extraction/sim_air.py`, `sim_blank.py`, `configs/examples/experiment.sim-air.toml` (new) | the example lab's simulated air and blank runs |
| `tests/sim/test_linear_flow.cpp`, `test_gas_network.cpp`, `test_sim_config.cpp`, `test_keyed_noise.cpp` (new); `tests/integration/test_lab_sim.cpp` (new) | tests |
| `docs/simulator.md` (new) | user guide |

---

### Task 1: Species and the exact linear solver

**Files:**
- Create: `libs/sim/include/pychron/sim/gas.hpp`, `libs/sim/include/pychron/sim/linear_flow.hpp`, `libs/sim/src/linear_flow.cpp`, `tests/sim/test_linear_flow.cpp`

(`libs/sim` and `tests/sim` glob their sources: no CMake edit.)

**Interfaces:**
- Produces, namespace `pychron::sim`:
  ```cpp
  enum class Species { Ar36, Ar37, Ar38, Ar39, Ar40, Active };
  inline constexpr std::size_t kSpeciesCount = 6;
  inline constexpr std::array<double, kSpeciesCount> kSpeciesMass{35.968, 36.967, 37.963, 38.964, 39.962, 28.0};
  inline constexpr std::array<std::string_view, kSpeciesCount> kSpeciesName{"Ar36", "Ar37", "Ar38", "Ar39", "Ar40", "active"};
  using Composition = std::array<double, kSpeciesCount>;      // per species; meaning set by the user (mbar, mbar L, mbar L/s)
  double total(const Composition& c);
  Composition scaled(const Composition& c, double factor);
  // Ratios to Ar36 (Ar36 == 1). air: Ar40 298.56, Ar38 0.1885, active 106 x the argon sum.
  Composition air_ratios();
  Composition cocktail_ratios();   // Ar40 298.56, Ar39 20, Ar38 0.1885, Ar37 0.5, active 0
  // The composition with these ratios whose Ar40 entry is `ar40`.
  Composition with_ar40(const Composition& ratios, double ar40);

  // dn/dt = K n + s for one species over N volumes, K and s constant.
  // K is given by its parts so it is symmetric in the sqrt(V) scaling by construction.
  struct FlowTerms {
    std::vector<double> volume;                                    // litres, > 0
    struct Link { std::size_t a, b; double conductance; };         // L/s between volumes a and b
    std::vector<Link> links;
    std::vector<double> loss;      // 1/s per volume: pump speed / V plus first-order loss
    std::vector<double> source;    // mbar L / s per volume: constant inflow plus pump base * speed
  };
  class LinearFlow {
   public:
    static Result<LinearFlow> make(FlowTerms terms);   // Config error on a bad size, index or non-finite/negative value
    // n(t + dt) from n(t); n in mbar L. dt >= 0.
    void advance(std::vector<double>& n, double dt) const;
    std::size_t size() const noexcept;
  };
  ```

**Algorithm (spec 3.3, 3.4):** with `D = diag(sqrt(V))`, `A = D^-1 K D` is symmetric: `A_ii = -(sum of conductances on i)/V_i - loss_i`, `A_ij = C_ij / sqrt(V_i V_j)`. Decompose `A = U Λ U^T` by cyclic Jacobi rotation (sweep until the off-diagonal norm is below `1e-14` times the Frobenius norm, at most 100 sweeps). All `λ <= 0`. Work in `y = U^T D^-1 n`, `g = U^T D^-1 s`:

```
λ_k < -eps:  y_k(t+dt) = -g_k/λ_k + (y_k(t) + g_k/λ_k) * exp(λ_k dt)
|λ_k| <= eps: y_k(t+dt) = y_k(t) + g_k * dt            (eps = 1e-12 * max|λ|, or 1e-300 if all are zero)
```

then `n = D U y`, and clamp each `n_i` below zero by rounding to zero.

- [ ] **Step 1: Write the failing tests** in `tests/sim/test_linear_flow.cpp`:
  - `Gas.AirRatios`: `air_ratios()[Ar40] / air_ratios()[Ar36] == 298.56`; `Ar38/Ar36 == 0.1885`; `active == 106 * (Ar36 + Ar38 + Ar40)`; `with_ar40(air_ratios(), 2e-7)[Ar40] == 2e-7`.
  - `LinearFlow.TwoVolumesEquilibrateToTheVolumeWeightedMean`: V = {0.05, 0.15}, one link C = 0.1, n = {1e-6 * 0.05, 0}; after 100 s both pressures are `2.5e-7` to 1e-12 relative; after `t = tau = V1 V2 / ((V1 + V2) C) = 0.375 s` the difference in pressure is `1e-6 / e` to 1e-9 relative.
  - `LinearFlow.ConservesAmountWithNoLossOrSource`: 5 volumes in a chain, random conductances seeded `0x5eed`, 1000 random steps between 1e-6 s and 1e4 s: `sum(n)` constant to 1e-10 relative.
  - `LinearFlow.OneStepEqualsMany`: the chain above with losses and sources: `advance(n, 3600)` equals 3600 times `advance(n, 1)` to 1e-9 relative per volume.
  - `LinearFlow.PumpedVolumeDecaysToItsBase`: one volume V = 0.05, loss = S/V with S = 50, source = base * S with base = 1e-9, p0 = 1e-6: `p(t) = base + (p0 - base) exp(-t S / V)` at t = 0.0005, 0.001, 0.01 to 1e-9 relative.
  - `LinearFlow.AClosedRegionWithASourceGrowsLinearly`: two linked volumes, no loss, source 1e-12 on one: after 1e6 s `sum(n)` has grown by exactly `1e-6` (1e-9 relative) and both pressures are equal. (Review Focus 1)
  - `LinearFlow.StiffAndLongStepsStayFiniteAndNonNegative`: conductances 1e-6 and 1e3 in one chain, steps of 1e-6 s then 604800 s then 1e-6 s: every `n_i` finite and `>= 0`; with no loss or source the sum is conserved to 1e-9. (Review Focus 2)
  - `LinearFlow.RefusesBadTerms`: a zero volume, a link index out of range, a negative conductance, a NaN source: each `ErrorKind::Config`.
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R 'Gas\.|LinearFlow'`. Expected: build fails, no `gas.hpp`.
- [ ] **Step 3: Implement** `gas.hpp` (header only) and `LinearFlow`.
- [ ] **Step 4: Run** the same. Expected: PASS.
- [ ] **Step 5: Commit** `feat(sim): species, and the exact solution of a linear flow`.

---

### Task 2: `GasNetwork`

**Files:**
- Create: `libs/sim/include/pychron/sim/gas_network.hpp`, `libs/sim/src/gas_network.cpp`, `tests/sim/test_gas_network.cpp`

**Interfaces:**
- Consumes: Task 1.
- Produces (spec 3.2, 3.5):
  ```cpp
  using SpeciesRates = Composition;   // 1/s per species
  struct GasVolume { std::string name; double litres = 0.05; Composition initial{};     // mbar
                     Composition source_per_s{};    // mbar L / s
                     SpeciesRates loss_per_s{}; };  // 1 / s
  struct GasValve  { std::string name; double conductance = 0.1; };   // L/s for Ar40
  struct GasPump   { std::string volume; double speed = 50.0; double base = 1e-9;   // L/s, mbar total
                     bool nobles = true; bool active = true; };
  struct GasTopology { std::vector<GasVolume> volumes; std::vector<GasValve> valves; std::vector<GasPump> pumps;
                       std::vector<std::pair<std::string, std::string>> edges; };   // endpoints name volumes or valves
  class GasNetwork {
   public:
    static Result<GasNetwork> make(GasTopology topology);
    void advance(double seconds);
    void set_valve(std::string_view name, bool open);   // unknown name: ignored
    bool valve_open(std::string_view name) const;       // unknown: false
    bool has_volume(std::string_view name) const;
    Result<Composition> partial_pressures(std::string_view volume) const;   // mbar
    Result<double> pressure(std::string_view volume) const;                 // total, mbar
    Result<void> set_partial_pressures(std::string_view volume, const Composition& mbar);
    Result<void> inject(std::string_view volume, const Composition& mbar_litres);
    Result<void> add_volume(GasVolume volume);          // isolated; Config if the name exists
  };
  ```

**Rules:**
- All valves start closed.
- Volumes joined by an edge with no valve between them are merged at `make`: one internal volume whose size is the sum, initial pressures the volume-weighted mean, sources summed, losses volume-weighted, every pump kept. Each original name still answers `partial_pressures` (the merged value).
- A valve with exactly two volume neighbours (after merging) is a link of `conductance * sqrt(39.962 / mass)` for each species while open. A valve with fewer or more than two neighbours, or an edge naming an unknown node, is tracked with no physics.
- A pump adds, for the species it pumps (`nobles`: the five argon; `active`: `Active`), `loss += speed / V` and `source += base_share * speed`, where `base_share` is `base` split in the proportions of `air_ratios()` over the species that pump pumps.
- The six `LinearFlow`s are rebuilt in `set_valve` (when the state changes), `add_volume`, and at `make`; `advance` only applies them.
- Not thread-safe; knows no clock.

- [ ] **Step 1: Write the failing tests** in `tests/sim/test_gas_network.cpp` (helper `air(p40)` = `with_ar40(air_ratios(), p40)`):
  - `ValvesStartClosedAndIsolate`: two volumes at different pressures, 1 h: unchanged.
  - `AnOpenValveEquilibratesEachSpecies`: tank 0.5 L of `air(2e-7)`, line 0.05 L empty, valve C = 0.1; after 60 s both hold `air(2e-7 * 0.5 / 0.55)` to 1e-9 relative per species.
  - `LighterGasArrivesFirst`: the same, at `t = 0.05 s`: downstream `Ar36/Ar40` exceeds `1/298.56` by the factor the two exponentials imply (assert between 1.0001 and 1.06 times), and equals it to 1e-9 after 60 s.
  - `APipetteDeliversTankPressureTimesItsVolume`: tank 1 L `air(1e-6)`, pipette 1e-4 L, line 0.1 L; valves `T` (tank-pipette), `L` (pipette-line). Sequence: open T, 30 s, close T, open L, 30 s, close L. The line's Ar40 pressure equals the value the test computes from the two equilibrations (tank with pipette by volume, then pipette with line by volume), to 1e-9 relative. Ten shots: the Ar40 delivered per shot declines geometrically by `1 / (1 + 1e-4)` to 1e-6 relative (allowing for what stays in the pipette).
  - `ATankValveLeftOpenEmptiesTheTank`: open T and L together, 600 s: tank, pipette and line share one pressure, the amount-weighted mean. (Review Focus 5)
  - `APumpTakesAVolumeToItsBase`: volume 0.05 L at `air(1e-6)` with `GasPump{speed 50, base 1e-9}`: after 1 s total pressure is `1e-9` to 1e-6 relative, in air proportions.
  - `AGetterRemovesActiveGasOnly`: `GasPump{nobles = false, base = 0, speed = 1}`: after 10 s `Active` is below 1e-30 and each argon is unchanged to 1e-12.
  - `OutgassingRaisesAnIsolatedVolumeLinearly`: `source_per_s = scaled(air(1), 1e-13)`, 0.05 L, 1000 s: Ar40 pressure rose by `1e-13 * 1000 / 0.05` to 1e-9 relative. (Review Focus 1)
  - `FirstOrderLossAndMemory`: one volume with `loss_per_s[Ar40] = 2e-5` and `source_per_s[Ar40] = q`: tends to `q / (2e-5 * V)`; from 100 times that it falls, from zero it rises.
  - `VolumesJoinedWithoutAValveAreOne`: A - B by a direct edge, B - valve - C: `partial_pressures("A") == partial_pressures("B")` always; opening the valve equilibrates A+B with C by total volume.
  - `InjectAddsExactlyTheAmount`: `inject("line", c)` raises each partial pressure by `c[i] / V`.
  - `AValveWithNoPhysicsIsTracked`: a valve named in `valves` with no edges: `set_valve` then `valve_open` is true; nothing else changes; an unknown name is ignored. (Review Focus 4)
  - `RefusesBadTopology`: duplicate volume name, zero volume, pump on an unknown volume, negative conductance: `ErrorKind::Config` naming the item.
  - `ManyTogglesStayPhysical`: 10 000 random valve toggles and steps (seed `0x5eed`) on a seven-volume network with a pump and a leak: every pressure finite and `>= 0` throughout.
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R GasNetwork`. Expected: build fails.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the same with `--repeat until-fail:3`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(sim): a gas network of volumes, valves, pumps and sources`.

---

### Task 3: `SimSystem` on the network; noise that does not depend on call order

**Files:**
- Create: `libs/sim/include/pychron/sim/keyed_noise.hpp`, `tests/sim/test_keyed_noise.cpp`
- Modify: `libs/sim/include/pychron/sim/sim_system.hpp`, `libs/sim/src/sim_system.cpp`, `tests/sim/test_sim_system.cpp`, and any test that reads a mean pressure straight after opening a valve (`grep -rln "sim()\|SimSystem" tests`)

**Interfaces:**
- Consumes: Task 2.
- Produces:
  ```cpp
  // keyed_noise.hpp: the same (seed, key, tick) always gives the same draw.
  std::uint64_t keyed_bits(std::uint64_t seed, std::string_view key, std::int64_t tick);   // splitmix64 over a hash of the three
  double keyed_gauss(std::uint64_t seed, std::string_view key, std::int64_t tick);         // mean 0, sigma 1 (Box-Muller)

  // SimSettings gains (all with these defaults; existing fields keep their meaning):
  double default_volume_cc = 50.0;
  double valve_conductance = 0.1;                       // L/s, Ar40
  double outgassing = 5e-13;                            // mbar L / s of Ar40 per litre of volume, air ratios, see below
  double outgassing_active = 1e-10;                     // mbar L / s per litre
  std::map<std::string, Composition> compositions;      // by volume: initial partial pressures, mbar
  std::map<std::string, double> conductances;           // by valve
  std::map<std::string, double> leaks;                  // by volume: mbar L / s of air (Ar40 entry)
  std::map<std::string, bool> getters;                  // by volume: a pump of active gas only
  // SimSystem gains:
  Result<Composition> partial_pressures(std::string_view volume) const;
  Result<void> set_composition(std::string_view volume, const Composition& mbar);
  Result<void> inject(std::string_view volume, const Composition& mbar_litres);
  ```
- `SimTopology`/`SimVolume` keep their shape; `SimSystem` builds the `GasTopology` from topology plus settings: `cc / 1000` litres (or `default_volume_cc` where a stage has none: `SimVolume::cc == 0` means unset; `topology_of` in Task 4 stops defaulting to 1.0); `initial_pressures[name]` or `default_pressure` as a total in air proportions unless `compositions[name]` is given; `SimPump{base, tau}` as `speed = V / tau`.
- Time: `advance_locked()` calls `network_.advance(seconds since last_)`.
- `set_pressure(volume, value)` scales the volume's present composition to that total (air proportions if it is empty).
- `gauge_reading` = `pressure * (1 + noise * keyed_gauss(seed, volume, nanoseconds since the SimSystem was built))`. `rng_` goes.
- Gauges not on the canvas still get an isolated volume (`add_volume`), as now.

**Behaviour change (spec 7.1):** equilibration takes time. Tests that open a valve and read at once advance the clock by 30 s of clock time first, or set `valve_conductance = 1e6` where the valve is incidental.

- [ ] **Step 1: Write the failing tests:**
  - `KeyedNoise.SameKeySameDraw`, `KeyedNoise.DifferentKeyOrTickDiffers`, `KeyedNoise.GaussHasUnitVariance` (100 000 ticks: mean within 0.01, variance within 0.02 of 1).
  - `SimSystem.EquilibrationTakesTheValvesTimeConstant`: two 50 cc volumes at 1e-6 and 1e-9, default conductance; at 0 s after opening they differ; after `0.25 s` (one time constant) the difference is `1/e` of the start to 1 %; after 30 s equal.
  - `SimSystem.GaugeReadingsDoNotDependOnCallOrder`: two `SimSystem`s on `ManualClock`s at the same time read volumes A then B, and B then A: the same two numbers.
  - `SimSystem.PumpDownKeepsItsForm`: `SimPump{base 1e-9, tau 5 s}` on a 50 cc volume at 1e-6: after 5 s, `base + (p0 - base)/e` to 1e-6 relative.
  - `SimSystem.PartialPressuresFollowTheComposition`, `SimSystem.SetPressureScalesTheComposition`, `SimSystem.InjectReachesTheGauge`.
  - `SimSystem.AnUnmodelledValveIsTrackedWithoutPhysics`. (Review Focus 4)
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R 'KeyedNoise|SimSystem'`. Expected: build fails (`keyed_noise.hpp`, `partial_pressures`).
- [ ] **Step 3: Implement**; update the existing tests per the behaviour change. For each existing test changed, keep its assertion and change only how it lets time pass.
- [ ] **Step 4: Run** `ctest --test-dir build/dev -R 'sim|Sim|Line|Gauge|Maxigauge|Xgs|Plc|Qtegra'`, then both full suites. Expected: PASS.
- [ ] **Step 5: Commit** `feat(sim): the simulated line carries gas by species through a network`.

---

### Task 4: Roles from the canvas; `sim.toml`

**Files:**
- Create: `libs/sim/include/pychron/sim/sim_config.hpp`, `libs/sim/src/sim_config.cpp`, `tests/sim/test_sim_config.cpp`, `configs/examples/sim.toml`
- Modify: `libs/sim/include/pychron/sim/sim_system.hpp` (`SimTopology`), `libs/systems/src/extraction_line.cpp` (`topology_of`, construction), `libs/systems/include/pychron/systems/extraction_line.hpp`, `tests/systems/test_extraction_line*.cpp` (the file that covers sim construction)

**Interfaces:**
- Produces:
  ```cpp
  // What a volume is to the gas, from the canvas stage's kind.
  enum class SimRole { Plain, Pump, Getter, Tank, Pipette, Spectrometer };
  struct SimVolume { std::string name; double cc = 0.0; SimRole role = SimRole::Plain; };   // cc == 0: unset

  // SimSettings gains:
  double tank_argon40 = 3e-5;            // mbar of Ar40 in a tank, air ratios, unless set
  double pipette_cc = 0.1;
  double pump_speed = 50.0;  double pump_base = 1e-9;  double getter_speed = 1.0;
  struct Source { double sensitivity = 1e12; double consumption = 2e-5; double memory_fa_per_s = 0.01; } source;  // spec 5.1
  struct DetectorBaseline { double baseline = 0.0; double drift_per_h = 0.0; };
  std::map<std::string, DetectorBaseline> detectors;
  std::map<std::string, Composition> named;              // [compositions.<name>], as ratios to Ar36

  // sim_config.hpp
  Result<SimSettings> load_sim_settings(const std::filesystem::path& file, const SimTopology& topology, SimSettings base = {});
  ```
- `topology_of` sets each `SimVolume::role` from `canvas::source_kind(stage)` (`Pump`, `Getter`, `Tank`, `Spectrometer`; a `[[pipette]]` element is `Pipette`; `Laser` and `None` are `Plain`) and leaves `cc` at 0 where the stage has no `volume`.
- `SimSystem` applies roles when it builds the `GasTopology` (spec 6.1): `Pump` gets `GasPump{pump_speed, pump_base}` unless `settings.pumps` has it; `Getter` gets `GasPump{getter_speed, 0, nobles = false}`; `Tank` starts at `with_ar40(air_ratios(), tank_argon40)` unless a composition is set; `Pipette` has `pipette_cc` unless sized; `Spectrometer` gets `loss_per_s = consumption` on the five argon and `source_per_s[Ar40] = memory_fa_per_s / sensitivity * litres`; every volume gets outgassing `litres * (with_ar40(air_ratios(), outgassing) with Active = outgassing_active)`, plus its leak.
- `SimSystem::spectrometer_volume() const -> std::optional<std::string>`: the first `Spectrometer` volume by name order; a second one is logged once as a warning through the line's logger when the line builds the sim. (Review Focus 3)
- `ExtractionLine`: `[sim] file = "<path>"` in the line's toml (relative to it), else a `sim.toml` beside it if one exists, is loaded into `options_.sim` before the `SimSystem` is built. An error from it fails the line's load with that error.
- `sim.toml` keys are exactly those in spec 6.2: `[defaults]` (`pressure`, `volume_cc`, `valve_conductance`, `outgassing`, `noise`, `seed`), `[compositions.<name>]` (species names as keys), `[volumes.<name>]` (`composition`, `argon40`, `pressure`, `leak`, `volume_cc`), `[valves.<name>]` (`conductance`), `[pumps.<name>]` (`speed`, `base`), `[spectrometer]` (`sensitivity`, `consumption`, `memory_fA_per_s`), `[detectors.<name>]` (`baseline`, `baseline_drift_per_h`). `composition` names a `[compositions.*]` table or the built-ins `air`, `cocktail`.

- [ ] **Step 1: Write the failing tests:**
  - `SimConfig.AnEmptyFileGivesTheDefaults`; `SimConfig.ReadsEveryKeyOfTheSpecExample` (the spec 6.2 example, each value asserted).
  - `SimConfig.RefusesAnUnknownVolumeValveOrPump`: three cases; the message contains the file name and the key (for example `volumes.nosuch`). `SimConfig.RefusesAnUnknownKey`, `SimConfig.RefusesANegativeOrNonFiniteNumber`, `SimConfig.RefusesAnUnknownComposition`. (Review Focus 3)
  - `SimSystem.ATankStartsFullOfAir`, `SimSystem.APumpStagePumps`, `SimSystem.AGetterStageRemovesActiveGas`, `SimSystem.TheSpectrometerStageConsumesAndRemembers` (static, Ar40 at 1e-8 mbar falls by `2e-5 * 600` over 600 s to 1 %; from zero it rises by `0.01 * 600 / 1e12` mbar).
  - `SimSystem.NoSpectrometerStageMeansNoSpectrometerVolume`; `SimSystem.TwoSpectrometerStagesTakeTheFirstByName`.
  - `ExtractionLine.LoadsSimTomlBesideTheLine`, `ExtractionLine.ABadSimTomlFailsTheLoadNamingIt`, `ExtractionLine.TheExampleLabLoadsWithItsSimToml`.
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R 'SimConfig|SimSystem|ExtractionLine'`. Expected: build fails.
- [ ] **Step 3: Implement**, and write `configs/examples/sim.toml` with every section present and commented, values equal to the defaults except what Task 7 tunes.
- [ ] **Step 4: Run** the same, then `ctest --test-dir build/dev -R 'sim|Sim|Line|integration'`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(systems): a simulated line takes its roles from the canvas and its numbers from sim.toml`.

---

### Task 5: `BeamModel`: a gas provider, a baseline, keyed noise

**Files:**
- Modify: `libs/sim/include/pychron/sim/spectrometer/beam_model.hpp`, `libs/sim/src/spectrometer/beam_model.cpp`, `tests/sim/test_beam_model.cpp`

**Interfaces:**
- Consumes: `keyed_noise.hpp`.
- Produces:
  ```cpp
  // BeamSettings
  std::function<std::vector<BeamGas>(TimePoint)> gas_at;   // empty: `gas`, as today
  // BeamDetector
  double baseline = 0.0;               // fA (Faraday) or cps (counter), added whatever the magnet position
  double baseline_drift_per_h = 0.0;
  // BeamModel
  void set_gas_provider(std::function<std::vector<BeamGas>(TimePoint)> provider);   // replaces settings_.gas_at
  Result<void> set_baseline(std::string_view det, double baseline, double drift_per_h);
  ```
- With a provider, `true_signal_locked` uses `gas_at(t)` and ignores `BeamGas::rate_per_s`; `peak_center` looks isotopes up in `gas_at(clock_.now())`, and when the provider's list lacks an isotope it falls back to `settings_.gas` for the mass (the peak position does not depend on how much gas there is). The provider is called with the model's mutex held: it must not call back into the model.
- Reading = `(true signal + baseline + baseline_drift_per_h * hours since the model was built) + noise`. A protected detector or a blanked beam still reads baseline plus noise.
- Noise: Faraday `sigma * keyed_gauss(seed, detector, tick)`; counter: a `std::mt19937_64` seeded with `keyed_bits(seed, detector, tick)` drawn once through `std::poisson_distribution`. `tick` is nanoseconds of `t` since the model was built. `rng_` goes.

- [ ] **Step 1: Write the failing tests:**
  - `BeamModel.WithAProviderTheSignalFollowsIt`: provider returns Ar40 at `1000 + 10 * seconds`; on the Ar40 peak the reading follows it to the noise.
  - `BeamModel.WithoutAProviderNothingChanges`: the existing fixed-gas expectations still hold (the existing tests are the check; add one that `rate_per_s` still acts).
  - `BeamModel.OffPeakReadsTheBaseline`: baseline 50, magnet at mass 34.2: mean of 1000 readings is 50 within 3 standard errors; with drift 2 /h, two hours later it is 54.
  - `BeamModel.AReadingDependsOnDetectorAndTimeNotOnOrder`: two models, same seed; read H1 then CDD at t, versus CDD then H1: identical values. Reading the same detector twice at the same instant gives the same value.
  - `BeamModel.PeakCenterKnowsAnIsotopeTheProviderOmits`.
  - `BeamModel.ABlankedBeamReadsBaselineOnly`.
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R BeamModel`. Expected: build fails.
- [ ] **Step 3: Implement.** Existing tests that assert an exact noisy value from the old generator are re-pinned to the new one; tests that assert statistics are untouched.
- [ ] **Step 4: Run** `ctest --test-dir build/dev -R 'BeamModel|SimSpectrometer|Spectrometer|MeasurementSim'`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(sim): the beam reads its gas from a provider, and has a baseline`.

---

### Task 6: The line's gas reaches the beam

**Files:**
- Modify: `libs/sim/include/pychron/sim/sim_system.hpp`, `libs/sim/src/sim_system.cpp`, `libs/systems/include/pychron/systems/spectrometer/bringup.hpp`, `libs/systems/src/spectrometer/bringup.cpp`, `apps/elctl/src/exp.cpp`, `apps/pychron-ui/src/main.cpp`, `libs/experiment/src/lab/session.cpp` (only if it builds a spectrometer itself: `grep -n "load_spectrometer_for_app\|BeamModel" libs/experiment -r`)
- Test: `tests/sim/test_sim_system.cpp`, `tests/systems/test_spectrometer_bringup.cpp`

**Interfaces:**
- Produces:
  ```cpp
  // SimSystem: peak-top signal (fA) of each argon isotope in the spectrometer volume at `t`:
  // partial pressure * settings.source.sensitivity. Empty function when there is no spectrometer volume.
  std::function<std::vector<BeamGas>(TimePoint)> beam_gas();
  const SimSettings& settings() const noexcept;
  // SpectrometerBringup
  sim::SimSystem* line_sim = nullptr;   // with sim_beam_from_table: the beam reads this line
  ```
- `beam_gas()`'s function advances the model to `t` under the `SimSystem` mutex (`t` earlier than the model's time reads the present state) and returns the five isotopes with `rate_per_s = 0`.
- Bringup: when `sim_beam_from_table` and `line_sim` are set and `line_sim->beam_gas()` is not empty, the registered `BeamModel` gets it as provider, and each `settings().detectors` entry is applied with `set_baseline` after the detectors exist (unknown detector names in `sim.toml` are a `Config` error naming the key).
- `exp.cpp` builds its own `BeamModel` (about line 205): the same two steps there. `main.cpp` passes `line_sim = line->sim()`.
- Lifetime: the provider holds a pointer to the line's `SimSystem`. The beam model must be released before the line: `exp.cpp`'s `BeamGuard` already runs before the line is destroyed; check `main.cpp`'s teardown order (`BeamModelRegistry::global().clear()` before the line goes) and fix it if not.
- Lock order: `BeamModel` mutex, then `SimSystem` mutex. `SimSystem` never calls a `BeamModel`.

- [ ] **Step 1: Write the failing tests:**
  - `SimSystem.BeamGasIsPartialPressureTimesSensitivity`; `SimSystem.BeamGasIsEmptyWithoutASpectrometerStage`. (Review Focus 3)
  - `SpectrometerBringup.ALineSimFeedsTheBeam`: example line and example simulated spectrometer on one `ManualClock`; set the spec volume to `air(1e-8)`; the Ar40 detector on peak reads about `1e-8 * 1e12 = 1e4` fA (within 1 %); set it to zero: reads baseline.
  - `SpectrometerBringup.WithoutALineSimTheBeamIsAsBefore`.
  - `SpectrometerBringup.AnUnknownDetectorInSimTomlIsRefused`.
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R 'SimSystem.BeamGas|SpectrometerBringup'`. Expected: build fails.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** both full suites. Expected: PASS. Existing simulated experiments now measure the line's gas: where an existing test asserted a signal level that came from the fixed argon defaults, find out what the line now delivers in that test and decide per test whether it should set a composition, use a provider-less spectrometer, or assert the new physics; record each in the commit body.
- [ ] **Step 5: Commit** `feat(systems): a simulated spectrometer measures the gas in the simulated line`.

---

### Task 7: The example lab runs air and blanks

**Files:**
- Create: `configs/examples/scripts/extraction/sim_air.py`, `configs/examples/scripts/extraction/sim_blank.py`, `configs/examples/experiment.sim-air.toml`, `tests/integration/test_lab_sim.cpp`
- Modify: `configs/examples/sim.toml`, `configs/examples/canvas.toml` and `extraction_line.toml` only if the pipette cannot be operated as wired (the pipette `air` sits between valves `P1` and `P2`; confirm with the canvas's connections), `tests/integration/CMakeLists.txt` (sources are listed there)

**Interfaces:**
- Consumes: Tasks 1 to 6; `LabSession` on `VirtualClock` as `tests/integration/test_lab_session.cpp` builds it.
- `sim_air.py`: isolate prep from the turbo; load the pipette (open the tank-side valve, wait, close it); deliver (open the line-side valve, wait, close it); leave the gas in prep for the plan's inlet. `sim_blank.py`: the same timing with the pipette valves untouched. Both take their waits from `duration` and the script header's `eqtime`, as `sim_extract.py` does.
- `experiment.sim-air.toml`: blank, air, air, air, blank, on the existing simulated plan (`plans/sim_multicollect.toml`).
- Tuning: adjust `configs/examples/sim.toml` (tank `argon40`, `outgassing`, pipette and stage volumes) so that in this lab one air run's Ar40 time-zero intercept is between 1e4 and 1e5 fA and a blank's is between 5 and 200 fA. Record the final numbers in `sim.toml` comments. Change the library defaults only if the example needs nothing else.

- [ ] **Step 1: Write the failing tests** in `tests/integration/test_lab_sim.cpp` (fixture on `VirtualTimeTest`, unlimited speed, a scratch copy of the example lab; each test runs a queue through `LabSession` and reads the collected analyses as `test_lab_session.cpp` does):
  - `LabSim.AnAirRunsSignalRisesAtTheInletAndFallsAtThePump`: in the Ar40 series, the reading before the inlet opens is baseline; after the inlet it rises with a time constant within 20 % of `V_prep V_spec / ((V_prep + V_spec) C_inlet)` from the configured values; during the static measurement its slope is negative and within 30 % of `-consumption * signal`; after the post-measurement pump-out it is back within 5 sigma of baseline.
  - `LabSim.AirHasTheAtmosphericRatio`: the ratio of the Ar40 and Ar36 time-zero intercepts, each baseline-corrected with the run's own baseline (fit them with `libs/reduction`'s linear fit as the reduction tests do), is within 2 % of 298.56.
  - `LabSim.ABlankIsSmall`: a blank's Ar40 intercept is under 1 % of an air run's, and above zero.
  - `LabSim.SuccessiveShotsDeclineWithTheTank`: three air runs: `I2/I1` and `I3/I2` equal the depletion factor computed from the configured tank and pipette volumes within 3 standard errors of the intercepts.
  - `LabSim.AScriptThatSkipsThePipetteMeasuresABlank`: an air run whose script is `sim_blank` matches `ABlankIsSmall`.
  - `LabSim.ATankValveLeftOpenDrainsTheTank`: a script variant that opens both pipette valves together: that run's Ar40 is far above a normal shot (more than 10 times), and the next normal shot is smaller than the first run's by more than the depletion factor; no error. (Review Focus 5)
  - `LabSim.TheSameQueueGivesTheSameNumbers`: the queue run twice in fresh sessions with the same seed: every reading of every isotope identical.
  - `LabSim.FiveRunsTakeNoRealTime`: the five-run queue finishes in under 30 s of real time.
- [ ] **Step 2: Run** `ctest --test-dir build/dev -R LabSim`. Expected: build fails (no scripts, no queue).
- [ ] **Step 3: Write** the scripts, the queue and the tuning. If `AirHasTheAtmosphericRatio` misses 2 %, find which physical term is responsible (inlet not yet equilibrated at time zero, consumption, baseline) and fix the script's equilibration time or the plan's settings; do not widen the tolerance without recording why in the report.
- [ ] **Step 4: Run** `ctest --test-dir build/dev -R LabSim --repeat until-fail:5`, the same once in `build/tsan`, then by hand: `build/dev/apps/elctl/elctl --sim exp run <scratch>/experiment.sim-air.toml --sim-speed max --data <scratch>/data` and record the five Ar40 intercepts.
- [ ] **Step 5: Commit** `feat(configs): the example lab runs simulated air shots and blanks`.

---

### Task 8: Documentation and close

**Files:**
- Create: `docs/simulator.md`
- Modify: `AGENTS.md`, `README.md` (where it lists the docs), `docs/superpowers/specs/2026-10-06-lab-simulator-design.md`

- [ ] **Step 1: Write `docs/simulator.md`** (user guide, in the style of `docs/export.md`): what `--sim` models and what it does not (spec 1, with laser and furnace release and calibrated ages named as not modelled); how roles come from the canvas; every `sim.toml` key with its unit and default (generated by reading `sim_config.cpp`, not the spec); how to run the example air queue with `elctl` and with `pychron-ui --sim --sim-speed 50`; what to expect on the gauges and the signal; `--sim-speed`.
- [ ] **Step 2: Add to `AGENTS.md`** under "Build and test", one bullet: where the simulator's physics lives (`libs/sim` `GasNetwork`, pure and clock-free; `SimSystem` holds time and devices), that readings are keyed by (seed, name, time) so a simulated run is reproducible and no code may draw simulator noise from a shared generator, that `sim.toml` keys are validated and documented in `docs/simulator.md`, and that a new gas source enters through `SimSystem::inject` or a source term, not through the beam.
- [ ] **Step 3: Bring the spec in line** with what was built: section by section against the code (interfaces in 3.2 and 3.5, defaults in 6.2, the test list in 8), `Status: Implemented`, and a short "Not yet" section naming laser and furnace release.
- [ ] **Step 4: Verify.** Both trees built, both full suites; `build/asan` (ASan and UBSan, OpenCV off) full suite; `ctest --test-dir build/dev -R 'Gas|LinearFlow|SimSystem|SimConfig|BeamModel|LabSim' --repeat until-fail:10`. Expected: all PASS, no sanitizer report.
- [ ] **Step 5: Commit** `docs: the simulated lab, for users and for developers`.
- [ ] **Step 6: Land, when the owner says so.** Rebase on `origin/develop`, rerun Step 4's suites, merge into `develop`. No pull request.
