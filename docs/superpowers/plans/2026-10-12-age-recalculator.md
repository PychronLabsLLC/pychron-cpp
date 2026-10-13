# Age Recalculator Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Status:** draft written by an unattended run on 2026-10-12 from the spec; not yet read by the owner. Two things need the owner before Task 5 (see "Before starting").

**Goal:** Bring published K-Ar and 40Ar/39Ar ages from the decay constants, K abundance and monitor age they were published with to another set, each with an internal and an external uncertainty and a record of every value used, from `elctl recalc`.

**Architecture:** A Qt-free core in `libs/reduction` (`recalc.hpp`) recovers the quantity the laboratory measured (`R = F / F_m` for 40Ar/39Ar, `X = 40Ar* / K` for K-Ar) and computes the age again from it with `UFloat`, so each target parameter is one variable for the whole calculation. Monitors are resolved through a `MonitorLookup` function, so the core does not know the library. A parameter library (`recalc_library.hpp`) holds named decay constants, abundances and monitors, built in and from a user's TOML file. `elctl recalc` reads flags or a CSV, calls the core and writes a CSV that is also the change log.

**Tech Stack:** C++20, `pychron::reduction` (`Measured`, `UFloat`, `AgeUnits`, `Result`), tomlplusplus, GoogleTest.

**Spec:** `docs/superpowers/specs/2026-10-08-age-recalculator-design.md`. Read it first; section numbers below refer to it. Where this plan and the spec disagree, the spec is right and the plan is corrected in the same commit.

## Before starting

1. **The library's numbers come from the papers, not from memory and not from a program.** Spec section 5: every built-in value is transcribed from the primary paper that reported it, and section 2 forbids consulting ArAR. An agent that does not have the paper in front of it does not write the value. Task 5 therefore needs the owner to supply, for each entry of the intended first set, either the paper or the value with its page or table (Aldrich and Wetherill 1958; Steiger and Jäger 1977; Min et al. 2000; Renne et al. 2010, 2011; Nier 1950; Garner et al. 1975; Renne et al. 1998; Kuiper et al. 2008; Rivera et al. 2011). The Steiger and Jäger decay constants are the exception: they are already in the tree (`libs/reduction/src/arar_types.cpp:54-55`) and the library entry is tested equal to them.
2. **The independent test values** of spec section 7 (Dalrymple 1979, Table 2; published recalculations between FCs ages) are also literature numbers. Tasks 2 and 3 are written so that they land without them (identity, round trip, hand-derived partials) and Task 4 adds the literature rows when the owner has given them.
3. The spec's two open questions (section 9) do not block this plan: neither a monitor defined by measured 40Ar* and K, nor a covariance field, is built here.

## Global Constraints

- Qt-free: nothing here includes a Qt header. `libs/reduction` gains no dependency except tomlplusplus for the user library file (already a dependency of the tree; check `libs/reduction/CMakeLists.txt` and link it there if it is not yet).
- Errors are values: every function that can fail returns `pychron::Result<T>`; nothing throws. A failure names what was wrong (`unknown monitor 'FCs:x'`, `monitor chain has a cycle: A -> B -> A`).
- `ReductionConstants` and the presets in `arar_types.cpp` are not changed.
- Units: decay constants in 1/a; ages inside the core in years; `PublishedAge::units` says what the caller's numbers are and the result comes back in the same units. `Measured::error` is 1 sigma.
- The source set enters at nominal values only (spec 3.4). The target set's parameters are `UFloat::variable`s made once per calculation and passed down, never made again inside a helper: that is what counts a shared decay constant once.
- No value from ArAR, and no look at it (spec section 2).
- CSV written by the command is RFC 4180, every row the width of the header, and a file written with `--out` goes through `pychron::mark_as_user_file` (`libs/core` `user_file.hpp`).
- No number parsing with `sscanf` or its kind (MSVC C4996): `std::from_chars`, as the other commands do.
- Commits: Conventional Commits with the component as scope (`feat(reduction): ...`, `feat(elctl): ...`, `docs: ...`).
- Before each commit that changes C++: `python3 tools/quality_check.py` (exit 0), then the tests named in the task. Before the branch is merged: `python3 tools/compiler_sweep.py`, full `cmake --build --preset dev` and `ctest --preset dev`.
- Build and run:

  ```bash
  cmake --build --preset dev --target pychron_reduction_tests
  build/dev/tests/reduction/pychron_reduction_tests --gtest_filter='Recalc*'
  cmake --build --preset dev --target pychron_elctl_tests
  build/dev/apps/elctl/tests/pychron_elctl_tests --gtest_filter='RecalcCmd*'
  ```

  (Take the second binary's path from the build tree if it differs.)

## File Structure

| File | Responsibility |
|---|---|
| `libs/reduction/include/pychron/reduction/recalc.hpp` | Types of spec section 4; `resolve_monitor`, `recalculate_arar`, `recalculate_kar`. |
| `libs/reduction/src/recalc.cpp` | The method of spec section 3. |
| `libs/reduction/include/pychron/reduction/recalc_library.hpp`, `src/recalc_library.cpp` | Named entries with citations, built in and from TOML; a `MonitorLookup` over them. |
| `tests/reduction/test_recalc.cpp`, `test_recalc_library.cpp` | Core and library tests (globbed by `tests/reduction/CMakeLists.txt`). |
| `apps/elctl/src/recalc.hpp`, `recalc.cpp` | `elctl recalc arar|kar|library` and `--in/--out`. No stub: needs no store. |
| `apps/elctl/src/cli.cpp` | Dispatch and the usage text. |
| `apps/elctl/tests/test_recalc_cmd.cpp` | Command tests through `elctl::testing::run_raw`. |
| `docs/recalc.md` | User guide (spec section 8). |
| `docs/superpowers/specs/2026-10-02-arar-reduction-design.md` | Its Phase 2 row `convert_age` points to `docs/recalc.md`. |

---

### Task 1: Types and monitor resolution

**Files:** create `recalc.hpp`, `recalc.cpp`, `tests/reduction/test_recalc.cpp`.

- [ ] **Step 1: tests first** (`RecalcMonitor` suite):
  - an absolute monitor resolves to its age whatever the decay constants;
  - an intercalibrated monitor resolves by spec 3.1 from the monitor it names (`t = ln(1 + R (e^{λ t_m} - 1)) / λ`), checked against the formula written out in the test;
  - a K-Ar monitor resolves by spec 3.2: under its own decay and abundance set it returns its reference age exactly, under another it moves;
  - a chain of three (intercalibrated to intercalibrated to absolute) resolves, and changing the absolute age moves the end of the chain;
  - an unknown name fails naming it; a cycle fails naming the chain.
- [ ] **Step 2:** the structs of spec section 4 exactly as written there (`DecayConstants`, `KAbundance`, the three monitor kinds, `MonitorDef`, `ParameterSet`, `PublishedError`, `PublishedAge`, `Recalculated`), `using MonitorLookup = std::function<Result<MonitorDef>(const std::string&)>`, and `resolve_monitor`. `resolve_monitor` needs the target abundance for a K-Ar monitor: the spec's signature takes only `DecayConstants`; give it the `ParameterSet`'s abundance as an optional third value and correct the spec's signature in this commit (a K-Ar monitor with no abundance in the target set is the spec's "missing abundance" error).
- [ ] **Step 3:** internally the resolver works on `UFloat`s handed to it (decay constants and abundance already made variables by the caller) and keeps a list of the names on the path for the cycle check. The `Measured` overload makes the variables and calls it.
- [ ] **Step 4:** `quality_check.py`, `--gtest_filter='RecalcMonitor*'`, commit `feat(reduction): monitor ages resolved under a parameter set`.

### Task 2: 40Ar/39Ar recalculation

**Files:** `recalc.hpp`, `recalc.cpp`, `test_recalc.cpp`.

- [ ] **Step 1: tests first** (`RecalcArAr`):
  - identity: equal source and target sets return the age and the internal uncertainty unchanged (to 1e-12 relative);
  - round trip A to B to A returns the original age and internal uncertainty;
  - `invariant` is `R` of spec 3.1 and `percent_change` is `100 (t - t0) / t0`;
  - internal uncertainty equals `|∂t/∂t0| σ_t0` with the partial derived by hand in the test, at a young age (1 Ma) and an old one (2000 Ma);
  - external uncertainty against hand-derived partials for `λ_e`, `λ_β` and an absolute monitor age;
  - shared decay constant counted once: with a K-Ar monitor, the external uncertainty differs from the sum in quadrature of the monitor's and the unknown's decay contributions taken separately, and equals the value from the partials with the sign kept;
  - a missing monitor in either set, a non-positive age and a non-positive argument of the logarithm each fail with their reason;
  - units: an age given in ka comes back in ka.
- [ ] **Step 2:** `recalculate_arar`. Source side at nominal values: resolve the source monitor with exact numbers, compute `R` as a `UFloat` whose only variable is the published age. Target side: make the target variables once, resolve the target monitor with them, compute `t`. Internal error: the variance of `t` from the age variable alone; external: the whole variance. `UFloat::derivative` or the terms give the split; do not compute the age twice with errors zeroed.
- [ ] **Step 3:** `quality_check.py`, `--gtest_filter='RecalcArAr*'`, commit `feat(reduction): 40Ar/39Ar ages recalculated between parameter sets`.

### Task 3: K-Ar recalculation and the `full` published error

**Files:** `recalc.hpp`, `recalc.cpp`, `test_recalc.cpp`.

- [ ] **Step 1: tests first** (`RecalcKAr`, `RecalcPublishedError`):
  - identity and round trip as in Task 2;
  - `invariant` is `X` of spec 3.2; a change of abundance alone and of decay constants alone each move the age as the formula written out in the test says;
  - external uncertainty against hand-derived partials for `λ_e`, `λ_β`, `A` and `W`;
  - a missing abundance in either set fails;
  - `PublishedError::Full`, both methods: the first-order contributions of the source monitor age and decay constants are subtracted in quadrature, checked against the number computed in the test; a published uncertainty smaller than those contributions fails with "negative remainder" and is not clamped to zero.
- [ ] **Step 2:** `recalculate_kar`, and the subtraction of spec 3.4 as one helper used by both functions: it takes the source-side contributions (computed with the source set's uncertainties) and the published error, and returns the internal part or the failure.
- [ ] **Step 3:** `quality_check.py`, `--gtest_filter='Recalc*'`, commit `feat(reduction): K-Ar ages recalculated, and a published error that already holds the monitor's`.

### Task 4: Literature values (needs the owner's numbers)

**Files:** `test_recalc.cpp`.

- [ ] **Step 1:** add the rows of Dalrymple (1979) Table 2 the owner supplied (old constants to IUGS 1976) as a table-driven test, each row with the page it came from in a comment, tolerance the table's own rounding.
- [ ] **Step 2:** the same for the published 40Ar/39Ar recalculations between FCs ages.
- [ ] **Step 3:** a row that does not pass is a finding, not a tolerance to widen: stop and report it.
- [ ] **Step 4:** commit `test(reduction): recalculated ages against Dalrymple (1979) and published FCs recalculations`.

If the numbers are not there yet, skip to Task 5's structure work and come back; do not write a value from memory.

### Task 5: Parameter library

**Files:** create `recalc_library.hpp`, `recalc_library.cpp`, `tests/reduction/test_recalc_library.cpp`.

- [ ] **Step 1: tests first** (`RecalcLibrary`):
  - one row per built-in entry pinning its value, its error and a non-empty citation (the table in the test is the transcription check: a reader compares it with the paper);
  - the Steiger and Jäger decay entry equals `constants_preset(...)`'s `lambda_e` and `lambda_b`;
  - names: `key` for decay constants and abundances, `mineral:key` for monitors; an unknown name fails naming it and the kind looked for;
  - a user TOML file adds an entry of each kind, overrides a built-in of the same name, and is refused, with the entry's name, when an entry has no citation, an unknown key or a monitor that is none of the three kinds;
  - `lookup()` gives a `MonitorLookup` the core resolves through, user entries included;
  - `entries()` lists everything, built in and user, for `recalc library`.
- [ ] **Step 2:** `struct Cited<T> { T value; std::string citation; bool user = false; }` and `class RecalcLibrary` with `static RecalcLibrary built_in()`, `Result<void> load_user(std::string_view toml, std::string_view file)`, `Result<Cited<DecayConstants>> decay(name)`, `abundance(name)`, `monitor(name)`, `MonitorLookup lookup() const`, and the listing. The TOML shape (say it in `docs/recalc.md` too):

  ```toml
  [decay.mylab2020]
  lambda_e = [1.0e-10, 1.0e-12]       # value, 1 sigma, 1/a
  lambda_b = [4.0e-10, 4.0e-12]
  citation = "..."

  [abundance.mylab2020]
  k40_fraction = [1.0e-4, 1.0e-6]
  atomic_weight = [39.0, 0.001]
  citation = "..."

  [monitor."FCs:mylab2020"]
  age = [10.0, 0.1]                   # Ma: an absolute monitor
  citation = "..."
  # or: reference_age = [...], decay = "sj1977", abundance = "sj1977"   (K-Ar)
  # or: r = [...], relative_to = "GA1550:renne1998"                     (intercalibrated)
  ```

  The numbers above are made up, of the right shape for the test of the parser, and are said to be made up in the test; they are not library values.
- [ ] **Step 3:** the built-in table, one entry at a time, each from the paper or the owner's note in front of the implementer, each with its row in Step 1's table in the same commit.
- [ ] **Step 4:** `quality_check.py`, `--gtest_filter='RecalcLibrary*'`, commit `feat(reduction): a cited library of decay constants, K abundances and monitor ages`.

### Task 6: `elctl recalc`, one age

**Files:** create `apps/elctl/src/recalc.hpp`, `recalc.cpp`, `apps/elctl/tests/test_recalc_cmd.cpp`; modify `apps/elctl/src/cli.cpp`.

- [ ] **Step 1: tests first** (`RecalcCmd`, through `run_raw`): `recalc` with no arguments and with an unknown subcommand is `kUsage` with the usage text; `recalc arar` and `recalc kar` with the flags of spec section 6 print the recalculated age, both uncertainties and the percent change, exit `kOk`; a missing or unknown `--from-*` / `--to-*` name is `kUsage` naming it; `--units ka`, `--sigma 2` (input and output both) and `--error-kind full` do what the spec says; `recalc library` lists every built-in with value and citation; `--library <file>` adds and overrides, and a bad file is `kUsage` with the file's name.
- [ ] **Step 2:** `int recalc_command(const std::vector<std::string>& args, Io io)` after the pattern of `export_command` (`apps/elctl/src/export.hpp`), the header carrying the usage as that one does; dispatch in `cli.cpp` beside `export` and a line in the top-level usage. `apps/elctl/CMakeLists.txt` globs sources: check that `recalc.cpp` is not caught by the store-only filter (line 38) and that no stub is needed.
- [ ] **Step 3:** `quality_check.py`, `--gtest_filter='RecalcCmd*'`, commit `feat(elctl): recalc, a published age under another set of constants`.

### Task 7: `elctl recalc --in ... --out ...`

**Files:** `apps/elctl/src/recalc.cpp`, `apps/elctl/tests/test_recalc_cmd.cpp`.

- [ ] **Step 1: tests first:** a CSV mixing K-Ar and 40Ar/39Ar rows published under different sets comes back with the input columns repeated and the result columns added (age, internal, external, percent change, and the value and citation of every source and target parameter used); a row whose source and target sets are equal comes back unchanged; a failing row keeps its place with the reason in `error` and empty results, the other rows are still written, and the exit code is `kFailed`; every row has the header's width; a quoted field with a comma and a doubled quote survives (RFC 4180); a missing column, an unknown `method` and an unreadable `--in` are reported with the line number; optional `units` and `error_kind` columns override the flags per row.
- [ ] **Step 2:** the reader and writer. Look first for the CSV writer `libs/processing` `report.cpp` uses for `elctl export` and reuse its quoting if it is reachable from here without pulling the store in; otherwise a small local one with the RFC 4180 test above. `mark_as_user_file` after the file is closed.
- [ ] **Step 3:** `quality_check.py`, `--gtest_filter='RecalcCmd*'`, commit `feat(elctl): recalc a table of published ages, with the values used beside each`.

### Task 8: Documentation

**Files:** create `docs/recalc.md`; modify `docs/superpowers/specs/2026-10-02-arar-reduction-design.md`.

- [ ] **Step 1:** `docs/recalc.md` in the user guide's voice: what it does; the method (spec section 3) with the two invariants; the limits of spec 3.5 word for word in substance; the library and the user file (the TOML shape of Task 5); the command with one worked example per method and the CSV columns; the citation of Mercer and Hodges (2016) and the references of section 3.
- [ ] **Step 2:** the reduction spec's Phase 2 row `convert_age` points to `docs/recalc.md`; the recalculator spec's status line says implemented, with the date, and gains an "As built" section for anything the code settled (Task 1's `resolve_monitor` signature at least).
- [ ] **Step 3:** commit `docs: recalculating published ages`.

## Final check (before merging the branch)

```bash
python3 tools/quality_check.py
python3 tools/compiler_sweep.py
cmake --build --preset dev
ctest --preset dev
```

No thread is added and no stored format is written, so neither the thread sanitizer run nor the Windows fixture check of `AGENTS.md` applies; the tests still run under `-DPYCHRON_SANITIZE=address,undefined` once before the merge, as for any new arithmetic.
