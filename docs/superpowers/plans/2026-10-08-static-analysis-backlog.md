# Static Analysis Backlog Plan

**Goal:** Bring the whole tree to zero findings under `tools/quality_check.py`,
so the check can be run on whole files (and, later, in CI) instead of only on
changed lines.

**Baseline:** 2026-10-08, Homebrew LLVM 23.1.3, cppcheck 2.22, 1,220
first-party files, compile databases from the `dev-ui` preset (moc files
generated) and from a build without persistence and scripting (the stubs).

- Before Phase 0 (`origin/develop` at `3b5cc07`): **3,896 findings** (3,768
  clang-tidy, 128 cppcheck), 18 files not analysed by clang-tidy.
- After Phase 0: **1,972 findings** (1,856 clang-tidy, 116 cppcheck), every
  file analysed.

To reproduce (about a quarter of an hour for the whole tree):

```bash
cmake --preset dev-ui
cmake --build build/dev-ui --target $(cmake --build build/dev-ui --target help | grep -oE '[A-Za-z0-9_-]+_autogen$')
cmake -S . -B build/dev-min -DCMAKE_BUILD_TYPE=Debug -DPYCHRON_PERSISTENCE=OFF -DPYCHRON_SCRIPTING=OFF
python3 tools/quality_check.py --json libs apps tests
```

## What the findings are

| Bucket | Before | After Phase 0 | What it is |
|---|---:|---:|---|
| 0. Configuration noise | 1,928 | 2 | The check is wrong for the code, not the code for the check |
| 1a. Path and lifetime defects | 53 | 53 | Analyzer and cppcheck: leaks, null, dangling, out of bounds |
| 1b. Bug-prone patterns | 514 | 514 | Unchecked optionals, exceptions escaping `noexcept`, empty catches, widening |
| 2. Mechanical | 771 | 771 | clang-tidy has a fix-it: `emplace_back`, `contains`, `starts_with`, casts |
| 3. Manual performance and C arrays | 630 | 632 | By-value parameters, missing `reserve`, `T x[N]` |

The two left in bucket 0 are cppcheck's `syntaxError` (Phase 0, task 4); the
two gained in bucket 3 are in files that were not analysed before.

By component, the largest: `apps/pychron-ui` 1,450 (1,113 of them one check,
see Phase 0), `tests/ui` 230, `tests/processing` 222, `libs/processing` 197,
`tests/systems` 136, `libs/experiment` 132, `apps/elctl` 108.

Half of the total is two checks firing where they should not. Phase 0 removes
them before any code is touched, so the later phases work on a list that
means something.

## Global Constraints

- Work on `chore/` or `fix/` branches cut from `develop`; one component per
  commit so a regression bisects to a library. A change that fixes a real
  defect is `fix(<component>):` with a test that fails without it; a mechanical
  one is `refactor(<component>):`; a config change is `chore(tools):`.
- Build and run the component's tests after every commit, on clang with
  `-DPYCHRON_SANITIZE=address,undefined`. gcc disagrees with clang about some
  of these rewrites: build gcc 14 at the end of each phase (AGENTS.md).
- A finding that is wrong for the code is silenced on its line,
  `// NOLINT(<check>): <why>` or `// cppcheck-suppress <id>`. A check is taken
  out of `.clang-tidy` only in Phase 0, and only with its reason written
  beside it.
- No test is skipped, disabled or weakened to clear a finding.
- Importer code (`libs/ingest`, `libs/dvc`): the rules of spec section 10
  hold. A rewrite there reruns the adapter's `OneHistoryOneResult` test.
- `--fix` output is read before it is committed. A fix-it compiles; it is not
  shown to preserve behaviour.

## Phase 0: make the list honest (config only, about 1,930 findings): done

- [x] **1. `cppcoreguidelines-owning-memory` off in Qt code (1,186).** Qt's
  parent owns a `new`'d child; the check cannot see that. Add
  `apps/pychron-ui/.clang-tidy` and `tests/ui/.clang-tidy` with
  `InheritParentConfig: true` and `Checks: '-cppcoreguidelines-owning-memory'`.
  Remove the `LegacyResourceConsumers` option from the root `.clang-tidy`:
  listing `QObject::connect` there was a mistake that added 486 of these.
  Two findings remain outside Qt (`libs/core`, `libs/scripting`): Phase 1.
- [x] **2. `bugprone-unchecked-optional-access` off in tests (621).** A test
  that dereferences `*UtcTime::parse("2015-...")` fails loudly if it is empty,
  which is what a test should do. Add `tests/.clang-tidy` (and one for
  `apps/elctl/tests`) turning it off. It stays on for `libs/` and `apps/`
  (196 findings, Phase 1).
- [x] **3. Decide `cppcoreguidelines-pro-type-vararg` (106).** Decided: off. All are the
  `printf` family (`snprintf`, `sscanf`, `fprintf`). Either turn the check
  off, since `-Wformat` under `-Werror` already checks the arguments, or keep
  it and convert to `std::format` / `std::from_chars` in Phase 3. Recommended:
  off; the 13 `sscanf`/`atoi`/`atof` conversions that matter are reported
  separately by `bugprone-unchecked-string-to-number-conversion`.
- [x] **4. cppcheck false positives (15).** Read and suppressed by id in
  `cmake/cppcheck.supp`, each with its reason: `localMutex` (7, a mutex that
  exists only to wait on a condition variable), `objectIndex` (6,
  `git_reader.cpp:561-571`, a pointer into a vector's elements).
  **Left open, 2:** `syntaxError` at `experiment_queue.cpp:174` and
  `ingest/src/writer.cpp:50`. Both are valid C++ that cppcheck 2.22 (the
  current release) cannot parse, and it checks nothing in those two files
  past that line. Not suppressed, because a suppression would hide that:
  Phase 1 either rewrites the two expressions so it can, or suppresses them
  per file and says the file is clang-tidy's alone.
- [x] **5. Files clang-tidy skipped (18).** Twelve headers no compiled source
  includes by the spelling the script looks for (`expected.hpp`,
  `catalog.hpp`, ...): they are reached through another header, so teach
  `sources_including` to follow one level, or check them through any source of
  their component. Six `*_stub.cpp` files are not compiled by `dev-ui`:
  configure a second build with `-DPYCHRON_PERSISTENCE=OFF
  -DPYCHRON_SCRIPTING=OFF` and run them from it.
- [x] **6. Accept a directory on the command line** (`quality_check.py
  libs/core`), and collect every source that fails to parse instead of
  stopping at the first, so a whole-tree run is one command.
- [x] **7. Rerun the baseline** and replace the table above: 1,972.

## Phase 1: defects (about 570 findings, each one read)

Read the code for every finding; the tool is right about the pattern and
often wrong about the consequence.

- [x] **1a, the path and lifetime findings: done.** 58 with the dead stores
  and the loops counted in `double`, which the first count had elsewhere.
  Every one was read.
  None was a defect that can happen. What they were:

  | Outcome | Count | Which |
  |---|---:|---|
  | Check off where it cannot work | 21 | `NewDeleteLeaks` in Qt code (6, parent-owned); `CallAndMessage` after `QVERIFY` (4); cppcheck `containerOutOfBounds` and `nullPointer` in tests (11: `ASSERT_TRUE(v.empty()) << v.front()`, `QCOMPARE` then index) |
  | Silenced on the line, with the invariant that makes it safe | 26 | `isotope_classifier.cpp` (samples not empty, so a vote exists), `meta_adapter.cpp`, `codec.hpp`, `log_hub.cpp` (`rethrow_exception` does not return), `sim_config.cpp`, `params.cpp`, `sim_drivers.cpp`, `host_state.cpp`, `project_import.hpp`, three `std::function` "leaks" and three "stack escapes", and others |
  | Code changed | 3 | `flow_layout.cpp` (the destructor names `FlowLayout::takeAt`), a dead store in `script_highlighter.cpp` and one in `test_number.cpp` |
  | cppcheck cannot parse the file | 2 | `experiment_queue.cpp`, `ingest/src/writer.cpp`: suppressed per file in `cmake/cppcheck.supp`, which says cppcheck checks nothing past that line there |
  | Left open | 6 | below |

  Left open, for a decision rather than a comment:
  - `libs/systems/.../canvas/canvas.hpp:160`: `Image : Located` declares a
    `path` (the picture's file) that hides `Located::path` (where in the TOML
    the element is, used in messages). Both work today because each reader
    names the type it means; a message written through `Image` would print
    the file. Renaming one is a change to the canvas model and its readers.
  - `libs/core/src/scheduler.cpp`: `~Scheduler()` calls `stop()`, which throws
    when a job destroys its own scheduler. From a destructor that is
    `std::terminate`. Silenced with that said beside it; whether it should
    log first is a design question.
  - Five loops counted in `double` (`brand.cpp` three, `timeline.cpp:182`,
    `test_sim_legacy_config.cpp:248`): with 1b task 4.

Then bucket 1b, in this order:

- [ ] **1. `bugprone-unchecked-optional-access` in `libs/` and `apps/` (196).**
  Each is a guard, a `value_or`, or an early return of the error. Largest
  share is `apps/pychron-ui`.
- [ ] **2. `bugprone-exception-escape` (64).** A `noexcept` function or a
  destructor that calls something that allocates or parses. Either it cannot
  throw in practice (say why, `NOLINT`) or the `noexcept` is wrong.
- [ ] **3. `bugprone-empty-catch` (20), eleven in `libs/core/src/log_hub.cpp`.**
  A logger swallowing its own failure is deliberate; each gets a comment
  saying so, or a counter. The others (`ingest/late_revision.cpp:455`,
  `persistence/tiny/db.cpp`, `metrics`) are read one by one.
- [ ] **4. Number handling (56).** `unchecked-string-to-number-conversion`
  (13: `sscanf`/`atoi`/`atof` in `elctl`, the NGX and Qtegra codecs,
  `legacy_line.cpp`, `store.cpp:801`) becomes `std::from_chars` with the error
  reported; `implicit-widening-of-multiplication-result` and
  `misplaced-widening-cast` (25) widen before the multiply;
  `init-variables` and `pro-type-member-init` (26) get initialisers;
  `float-loop-counter` (6) counts in integers.
- [ ] **5. `bugprone-nondeterministic-pointer-iteration-order` (7).**
  `flux_admin.cpp:201`, `flux_view.cpp:477`, `source.cpp:149`,
  `spectrum.cpp:157`, `time_series.cpp:278`, `switch_manager.cpp:193`,
  `reference_fit_window.cpp:247`. Sorting by address makes output order
  differ between runs. None is in the importer, where it is forbidden
  outright, but a report or a figure ordered this way is still wrong: sort by
  a stable key.
- [ ] **6. `concurrency-mt-unsafe` (30, 21 in tests).** `getenv`, `localtime`,
  `strerror` and the like. `libs/core/.../env.hpp` is the one place that
  should call `getenv`; route the rest through it or use the `_r` forms.
- [ ] **7. Casts and forwarding (71).** `pro-type-static-cast-downcast` (15),
  `pro-type-reinterpret-cast` (13), `pro-type-const-cast` (4),
  `missing-std-forward` (27), `move-forwarding-reference` (2,
  `serialize.cpp:729,766`), `misleading-capture-default-by-value` (9, all in
  `options_editor.cpp`: `[=]` capturing `this`).
- [ ] **8. The rest of 1b** (about 60 across a dozen checks): read each.
- [x] **9. cppcheck's two `syntaxError`s**: suppressed per file (1a above).

Order of components within Phase 1, by what a defect costs: `libs/core`,
`libs/persistence`, `libs/ingest`, `libs/dvc`, `libs/reduction`,
`libs/processing`, then devices and systems, then `apps/`, then tests.

## Phase 2: mechanical (771 findings, `--fix`)

One commit per component, `refactor(<component>): ...`, no behaviour change
intended, component tests run after each.

- [ ] Run `python3 tools/quality_check.py --fix <files of the component>`,
  read the diff, build, test, commit.
- Checks and counts: `modernize-use-emplace` 171, `readability-container-contains`
  96, `cppcoreguidelines-prefer-member-initializer` 90,
  `modernize-avoid-c-style-cast` 65, `modernize-use-integer-sign-comparison`
  56, `modernize-raw-string-literal` 47, `modernize-use-starts-ends-with` 46,
  `readability-inconsistent-declaration-parameter-name` 36,
  `performance-faster-string-find` 33, `readability-simplify-boolean-expr` 24,
  `modernize-use-auto` 22, `modernize-loop-convert` 16, and about 130 more
  across smaller checks.
- Watch for: `use-integer-sign-comparison` needs `<utility>` and changes
  which comparisons are signed; `prefer-member-initializer` can reorder
  initialisation relative to member declaration order; `raw-string-literal`
  in SQL and regex strings is easy to get subtly wrong; `--fix` applied to a
  header through two sources must not be applied twice (the script runs
  fixes serially for that reason).

## Phase 3: manual performance and C arrays (630 findings)

Lowest value per finding; do it component by component when that component
is being worked on anyway, not as a campaign.

- [ ] `modernize-avoid-c-arrays` (255): `std::array`. Leave the ones that
  mirror a wire format or a C API and say so.
- [ ] `performance-unnecessary-value-param` and cppcheck `passedByValue` (150).
- [ ] `performance-inefficient-vector-operation` (93): `reserve` before the
  loop.
- [ ] `performance-inefficient-string-concatenation` (64).
- [ ] The rest (about 70): `returnByReference`, `no-automatic-move`,
  `use-string-view`, `pass-by-value`.

## Done when

- [ ] A whole-tree run exits 0 on clang with both build configurations of
  Phase 0 task 5.
- [ ] Every `NOLINT` and `cppcheck-suppress` added carries its reason.
- [ ] Then, and not before: a CI job on pull requests into `main` running the
  check on whole files, so the count cannot grow back. Until then the
  changed-lines run in AGENTS.md is what keeps new code clean.
