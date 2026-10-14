# Queue directory: queues under `<lab>/queues/`, blocks under `queues/blocks/`

Status: draft 2026-10-13, not agreed. Written by unattended run for T041.
Follows owner's decisions in `2026-10-13-queue-format-survey.md` section 7
(format A: TOML stays; queues under `<lab>/queues/`, blocks under
`queues/blocks/`; no `.txt` importer, no autosave, no `rem`/`ex` copies, no
comment-preserving writer). Nothing built. Questions: section 8.

## 1. What is there now

- Queue file sits in lab directory beside `plans/`, `scripts/`,
  `conditionals/` (any name; `experiment.toml` by convention,
  `docs/user/07-configuration-reference.md:534`).
- Blocks: `<lab>/blocks/*.toml`, loaded by `load_lab`
  (`libs/experiment/src/lab/lab.cpp:148`); run factory's tooltip names that
  directory (`apps/pychron-ui/src/run_factory_panel.cpp:320`); layout list
  `libs/experiment/include/pychron/experiment/lab/lab.hpp:14`.
- `elctl exp`: `--lab` defaults to queue file's directory
  (`apps/elctl/src/exp.cpp:570`); with `--install`, a relative queue name
  not found from cwd is looked up in install's lab (`exp.cpp:565`).
- UI: Open and Save As start from last queue path
  (`experiment_window/last_queue`, `experiment_window.cpp:544`, `:553`),
  filter `*.toml`; `--queue <file>` at start (`apps/pychron-ui/src/main.cpp`).
- Setup: profile renders `experiment.toml` and `blocks/blank_pair.toml` at
  install root (`profiles/instrument-common/profile.toml:67`–`72`); checked
  by `tests/setup/test_profiles.cpp:81`, `:127`, `:217`,
  `tests/integration/test_install_e2e.cpp:241`,
  `apps/elctl/tests/test_setup_commands.cpp:54`; release smoke runs
  `elctl --install argus exp run experiment.toml`
  (`.github/workflows/release.yml:210`, `:235`, `:246`).
- Example lab `configs/examples`: `experiment.toml`, `experiment.laser.toml`,
  `experiment.sim-air.toml`, `blocks/blank_pair.toml`; read by tests
  (`tests/experiment/test_lab.cpp:30`, `test_lab_extraction.cpp:58`,
  `tests/integration/test_lab_session.cpp:170`, `:193`,
  `test_lab_sim.cpp:166`, `tests/ui/experiment_fixture.hpp:45`,
  `tests/ui/test_experiment_window.cpp:370`,
  `apps/elctl/tests/test_exp_commands.cpp`) and docs
  (`docs/user/01-getting-started.md`, `04-experiments.md`, `07-…`).

## 2. Layout

```
<lab>/
  queues/                 queue files (*.toml), any name
    blocks/               reusable run sequences (*.toml)
  plans/ scripts/ conditionals/ ...   unchanged
  data/                   unchanged (records, spool, executor_state.json)
```

- `lab.hpp` layout comment: `<lab>/queues/*.toml` and
  `<lab>/queues/blocks/*.toml` replace `<lab>/blocks/*.toml`.
- Only `*.toml` directly in `queues/` are queues; `queues/blocks/` is not a
  queue directory. No further subdirectories are read (a lab may make its
  own, for its own filing; Open dialog shows them, nothing lists them).
- `LabPaths` gains `queues` (`<lab>/queues`) and `blocks`
  (`<lab>/queues/blocks`): one place says where they are; UI, `elctl` and
  setup take them from there.

## 3. Rules

- Blocks are read from `<lab>/queues/blocks/`. A lab that still has
  `<lab>/blocks/` (installs made before this change, hand-made labs): its
  blocks are read too, and the lab reports one problem line, `blocks/ is
  read from its old place; move it to queues/blocks/`. When both hold a
  block of one name, the one in `queues/blocks/` is used and the problem
  line names it. Nothing is moved by the program. (Question 1.)
- A queue may live anywhere; `queues/` is where the program looks and
  saves first, not a requirement. A queue outside it runs as today.
- Lab of a queue, when not given (`--lab`, `--install`, UI's lab): queue's
  directory; if that directory is named `queues`, its parent. Same function
  for `elctl` and the UI (`lab_for_queue(path)`, `libs/experiment`
  `lab/lab.hpp`), tested once.
- With an install (or the UI's lab), a relative queue name not found from
  cwd is looked up in `<lab>/queues/`, then `<lab>/`: old installs and the
  release smoke command (`exp run experiment.toml`) keep working.
- Data directory stays `<lab>/data` (from the lab, never from the queue's
  directory).
- UI Open and Save As start in last queue's directory when there is one,
  else in `<lab>/queues/`; Save of a new queue proposes
  `<lab>/queues/<name>.toml` and makes the directory if missing.
  `last_queue` setting unchanged.
- Run factory's empty Block tooltip names `<lab>/queues/blocks`.
- Queue file format unchanged: no `schema_version` change, no key added.

## 4. Setup and example lab

- Profile (`profiles/instrument-common/profile.toml`): `to =
  "queues/experiment.toml"`, `to = "queues/blocks/blank_pair.toml"`.
  Rules of `libs/setup/AGENTS.md` apply (read before changing; shipped
  queue `ba, a, a, a, ba` unchanged).
- Existing installs are not rewritten: rule 3's fallbacks cover them.
- Example lab: `configs/examples/queues/experiment.toml`,
  `experiment.laser.toml`, `experiment.sim-air.toml`,
  `configs/examples/queues/blocks/blank_pair.toml` (moved with `git mv`).
  Every test and doc line of section 1 updated to the new paths in the same
  commit as the move. (Question 2.)
- Tests that build a lab in a temp directory and write `experiment.toml`
  at its root keep doing so where the test is about something else: a
  queue outside `queues/` is legal.

## 5. Docs

`docs/user/01-getting-started.md`, `04-experiments.md` (layout at `:25`,
commands at `:199`, `:884`, `:904`–`:920`), `07-configuration-reference.md`
(`:124`, `:534`–`:538`, setup section `:916`, `:995`), `08-elctl-reference.md`
(`--lab` default), `11-glossary.md` (Block entry, `:11`). `elctl` help in
`apps/elctl/src/cli.cpp:68`–`70` and `exp.hpp:10` (the `--lab` default).

## 6. Tests

- `tests/experiment/test_lab.cpp`: blocks from `queues/blocks/`; old
  `blocks/` read with its problem line; same name in both, new wins and is
  named; no `blocks` directory at all is no problem.
- `lab_for_queue` table: `<lab>/queues/x.toml` → `<lab>`;
  `<lab>/x.toml` → `<lab>`; `x.toml` → `.`; `<lab>/queues/sub/x.toml` →
  `<lab>/queues/sub` (no walk past one level).
- `apps/elctl/tests`: with `--install`, `exp validate experiment.toml`
  finds `queues/experiment.toml`, and still finds a root
  `experiment.toml` in an install made the old way; `--lab` default from a
  queue under `queues/`.
- `tests/setup/test_profiles.cpp`, `tests/integration/test_install_e2e.cpp`:
  rendered install has `queues/experiment.toml` and
  `queues/blocks/blank_pair.toml`, no root `experiment.toml`.
- `tests/ui`: Open's start directory is `<lab>/queues/` with no last queue;
  Save of a new queue creates `queues/`; `--queue experiment.toml` relative
  to the lab.

## 7. Order of work (for the plan)

1. `LabPaths::queues`/`blocks`, `lab_for_queue`, blocks from both places
   with the problem line (library, tests).
2. `elctl`: `--lab` default and install lookup through them (tests).
3. UI: dialogs' start directory, new queue save, tooltip, `--queue` lookup
   (UI tests).
4. Setup profile and install tests; example lab moved, tests and docs
   updated.

Each lands alone with the tree building and `ctest --preset dev` (or
`dev-ui` for 3) passing.

## 8. Questions for owner

1. Old `<lab>/blocks/`: read with a problem line and never moved
   (proposed), or moved once by the program (`elctl setup` or at lab load),
   or refused?
2. Example lab: move its queues into `configs/examples/queues/` (proposed:
   it is what new labs copy), or leave the example as it is?
3. UI: a list of the lab's queues (File > Open Recent, or a Queues pane
   from `queues/`) wanted now, or Open dialog only?

## 9. Out of scope

- Queue format (decided: TOML, section 7 of the survey).
- Legacy `.txt` queue import, autosave, `rem`/`ex` copies, comment-keeping
  writer (owner: not wanted).
- Step heat templates (`2026-10-13-step-heat-template-design.md`, draft:
  proposes `<lab>/step_heats/`, not under `queues/`).
- Moving `data/` or `executor_state.json`.
