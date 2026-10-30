# Simulated lab (`libs/sim`): agent rules

Part of the agent guide: root `AGENTS.md` applies in full, this adds what is
only this component's. Kept compressed, as that file is.

- Simulated lab's physics is `libs/sim` `GasNetwork` on `LinearFlow`: gas by
  species in volumes, solved exactly between valve events, pure and
  clock-free (standard library, `core/error.hpp` and `core/keyed_noise.hpp` only). `SimSystem` holds
  the clock, the mutex and the device sims and moves the network to
  `clock.now()` on each query. Noise on a reading is keyed by (seed, name,
  time) (`core/keyed_noise.hpp`: `keyed_gauss` for Faradays and gauges,
  `keyed_poisson` for counters, one key for one of the two), so a simulated
  run gives same numbers every time: no code draws simulator noise from a
  shared generator or a `<random>` distribution. Gas enters through
  `SimSystem::inject` or a volume's source term, never through the beam;
  `feed_beam_from_line` (`libs/systems` bringup) is the one place a beam is
  joined to a line, and the two share a clock that outlives the beam.
  Equilibration takes time: a test that opens a valve advances the clock
  before it reads, and zeroes outgassing where it asserts an exact hold. A
  `sim.toml` key is read and range-checked in `sim_config.cpp`, listed
  commented out with its default in `configs/examples/sim.toml`
  (`SimConfig.TheExampleFileIsTheDefaults` holds the two together) and given
  a row in the user guide, `docs/simulator.md`. Example lab's five tuned
  numbers are pinned by that test too, and a line number of `sim_extract.py`
  by the script-editor test (`tests/ui/test_script_editor.cpp`), so retuning
  the example or editing that script means editing those tests.
  `[defaults] seed` is the one seed of the lab: `feed_beam_from_line` gives
  it to the beam when it is not `SimSettings`' default.
