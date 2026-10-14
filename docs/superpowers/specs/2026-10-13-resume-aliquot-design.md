# A resumed step heat keeps its aliquot

Status: draft 2026-10-13, not agreed. Written by unattended run for T091.
Builds on branch `fix/step-heat-aliquot` (T090, `f56e037f`, not on
`develop`): this spec assumes that change. Nothing built. Questions:
section 7.

## 1. Problem

After T090, `AliquotAllocator` (`libs/experiment` `persist/persister.hpp`)
remembers per identifier the aliquot and step it last gave; a later step
(B after A) reuses that aliquot. Memory lives as long as the allocator,
one per session (`libs/experiment/src/lab/session.cpp:89`). Program
restarted mid-heating (crash, `elctl exp run --resume`, window's resume
offered by T024's spec): allocator empty, so B, C ... of a heating with no
fixed aliquot each get `next_aliquot()`: heating split again, the case
T090 fixed, on the resume path only.

## 2. Two places the answer could come from

- A. `executor_state.json` (`executor.cpp:463`): today row, identifier,
  run_id, state per run of the queue being run. Would need aliquot and step
  added, read back at resume, handed to the allocator. Knows only runs of
  that queue, in that data directory; a heating started in another queue
  (or before a queue was re-opened under another name) is not there.
- B. The persister: whoever hands out aliquots already knows the last one
  saved for an identifier. Store: `IStore::last_analysis(identifier)` gives
  `LastAnalysis{aliquot, increment}` (`libs/persistence/include/pychron/persistence/store.hpp:375`;
  `increment` is the step index, -1 for none). Files: `FilePersister`
  already scans `<root>/<identifier>/<identifier>-<aliquot><step>.json`
  for the highest aliquot (`persister.cpp:64`). Same truth the numbering
  comes from, whatever queue or session made the run.

Proposed: B. (Question 1.)

## 3. Interface

```cpp
// libs/experiment persist/persister.hpp
struct LastRun {
  int aliquot = 0;
  int step = -1;  // step_index of its step; -1: no step
};

class IAnalysisPersister {
  ...
  // The identifier's run with the highest aliquot, and within it the
  // highest step, among runs saved or waiting to be sent. nullopt: none,
  // or this persister cannot say (the default).
  virtual Result<std::optional<LastRun>> last_run(const std::string& identifier) {
    return std::optional<LastRun>{};
  }
};
```

- Default returns none: test fakes and `NoDatabasePersister`
  (`apps/pychron-ui/src/no_database_persister.cpp`) unchanged and keep
  T090's in-session behaviour.
- `FilePersister::last_run`: same scan as `highest_on_disk`, also reading
  the step letters after the aliquot digits (`step_index`); highest
  aliquot, then highest step in it.
- `StorePersister::last_run`: `store_.last_analysis(identifier)` mapped
  (`increment` → `step`), then records waiting in the spool
  (`persist::Spool`, as `next_aliquot` already reads them at
  `store_persister.cpp:49`–`70`) taken into account by the same order. A
  store that cannot be reached: the cached last answer when there is one
  (as `next_aliquot` does with `read_`), else the error.

## 4. Allocator rule

`AliquotAllocator::allocate`, no fixed aliquot, step index `s > 0`:

1. In-session memory for the identifier with `s` after its step: reuse
   (T090, unchanged).
2. Else, nothing remembered in this session for the identifier: ask
   `persister_.last_run(identifier)`. When it has a step `t` with
   `0 <= t < s`: reuse its aliquot and remember it. Otherwise
   `next_aliquot()` as now.
3. An error from `last_run`: the allocation fails with it (run does not
   start), as an error from `next_aliquot` does. (Question 2.)

- Step A or no step: never asks `last_run` (always a new aliquot), as now.
- Asked once per identifier per session: after it, memory answers.
- Holds after a restart, in another queue, and on another day: a heating
  whose B, C ... come in a later queue joins the aliquot whose last saved
  step is before them. Same rule as within a session (T090): a step after
  the last saved step of the identifier's newest aliquot.

## 5. Rules

- Never reuses an (identifier, aliquot, step) already saved: reuse only
  when the step is after the newest saved step of that aliquot.
- Store and file persisters answer the same for the same runs (one test
  for both, as `next_aliquot`'s tests do).
- No change to the store schema, the record format, `executor_state.json`
  or the queue file.
- `last_run` is a read; it never issues a number.

## 6. Tests

- `tests/experiment/test_persist.cpp`: `FilePersister::last_run` on
  `12345-3A.json`, `12345-3B.json`, `12345-2.json` → {3, B}; no files → none;
  a name with no step → step -1.
- Allocator table, each row a fresh allocator over a persister whose last
  run is {3, B}: C → 3; B → next free (B is not after B); A → next free;
  no step → next free. A failing `last_run` fails the allocation.
- `tests/experiment` store adapter (`pychron_experiment_store_tests`):
  `StorePersister::last_run` from the store, and from a record waiting in
  the spool; SQLite and PostgreSQL (`engines()`).
- Executor: queue of A, B, C, run stops after A; a new `Executor` and
  allocator over the same persister resumes at B: B and C get A's aliquot.
- `elctl` `--resume` test of the same shape (`apps/elctl/tests/test_exp_commands.cpp`).

## 7. Questions for owner

1. Source: the persister (proposed, B) or `executor_state.json` (A)?
2. `last_run` failing (store down at a run's start): fail the run
   (proposed, as `next_aliquot` failing does), or fall back to a new
   aliquot with a warning?
3. A heating continued on another day or in another queue joins the old
   aliquot (proposed, follows from B), or only within the queue that
   started it?

## 8. Out of scope

- Aliquots fixed at queue build time (legacy did that in the factory):
  this keeps assignment at run start.
- `NoDatabasePersister`, which refuses to run anything.
- Changing T090's in-session rule.
