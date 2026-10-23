# What an install ships (`libs/setup`, `profiles/`): agent rules

Part of the agent guide: root `AGENTS.md` applies in full, this adds what is
only this component's. Kept compressed, as that file is.

- What an instrument install ships is held together by
  `tests/setup/test_profiles.cpp`: `seed.toml`, `defaults.toml`, shipped
  `queues/experiment.toml` (`ba, a, a, a, ba`), example lab's `sim.toml` (example line only), laser files
  (`tray_maps/` and `patterns/` with every line; `cameras.toml` and
  `stage_calibrations/` with example line only: they name its `co2`), and the placeholder field
  tables of `profiles/qtegra` and `profiles/ngx`
  (`EveryInstalledPlanPutsItsIsotopesOnItsDetectors`: every isotope of a
  plan's hop on its detector within 0.01; simulated peak is flat within
  0.02). Retuning or editing any of them means editing that test.
- `tests/integration/test_install_e2e.cpp` (`pychron_install_e2e_tests`,
  with persistence only) runs a fresh argus install's shipped queue into its
  database. It pins run ids (`ba-01`, `a-01` to `a-03`, `ba-02`; second session
  numbers on) and airs' raw Ar40/Ar36 within 3 % of 303.2: air's 298.56
  over simulated counter's yield 0.9847, since no IC factor is applied.
  Change to simulator's air, counter, shipped plan or field tables shows
  there.
- Data-source change (`data_source.cpp`) writes credentials, install record,
  site entry; nothing else under root. Reconfigure must keep it:
  `DataSource*.AReconfigureKeepsTheNewSource`.
- Shipped examples (`configs/examples`, in macOS bundle) read-only to
  programs. Program run on them keeps runtime state (line state, corrections,
  camera scales, snapshots, default data, field tables) in per-user examples
  folder: `setup::lab_state_dir`, `setup::field_table_root`
  (spec `2026-10-22-example-lab-state-design.md`). New file a program writes
  beside a lab goes through `LabPaths::state()` or those functions, never
  `<lab>/...` direct. Tests copy examples with `pychron::testing::copy_lab`
  (`tests/support/example_lab.hpp`): new runtime state in examples gets a
  `.gitignore` line and a place in `is_lab_runtime_state`.
