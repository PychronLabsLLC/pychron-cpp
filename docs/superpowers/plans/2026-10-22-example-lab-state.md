# Example lab state: plan

Spec: `docs/superpowers/specs/2026-10-22-example-lab-state-design.md`.

1. Tests copy the examples without their runtime state
   (`tests/support/example_lab.hpp`, every fixture that copies
   `configs/examples`). Done: commit `test: a scratch copy of the examples
   leaves out runtime state`.
2. `default_examples_state_dir` beside `default_log_dir`
   (`libs/core` `log_dir.hpp`, `log_dir.cpp`). Tests: `tests/core/test_log_dir.cpp`,
   each OS, unset and empty variables.
3. `setup::lab_state_dir` (`libs/setup` `installer.hpp`, `installer.cpp`).
   Tests: `tests/setup` (examples root, folder under it, relative path,
   symlink, other lab, empty user folder).
4. `LabPaths::state_dir` and `state()`; `load_lab` puts corrections and
   camera scales there, `Lasers` its snapshots (`libs/experiment` `lab.hpp`,
   `lab.cpp`, `lasers.cpp`). Tests: `tests/experiment/test_lab.cpp`,
   `test_lab_extraction.cpp`.
5. `elctl exp run` and `elctl laser` use the state folder for the line state
   file, the lab, the default data folder and snapshots (`apps/elctl/src`).
   Test: `apps/elctl/tests/test_exp_commands.cpp`, a scratch examples folder
   named by `PYCHRON_EXAMPLES_DIR`, a scratch `HOME` and `XDG_STATE_HOME`:
   after `exp run --sim` the examples are byte-identical and the state is in
   the user folder.
6. `pychron-ui` does the same (`apps/pychron-ui/src/main.cpp`). Built and
   run under `dev-ui`.
7. Field tables (spec 3.5): `SpectrometerData::table_root`,
   `LabPaths::spectrometer_tables`, `setup::field_table_root` copying the
   shipped tables once. Tests: `tests/setup/test_install.cpp`; the elctl test
   of task 5 also checks the saved table is in the user folder.
