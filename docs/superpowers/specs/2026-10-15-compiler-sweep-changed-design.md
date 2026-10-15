# compiler_sweep.py: sweep only what a branch changed

Status: draft, 2026-10-15.

## 1. Problem

`tools/compiler_sweep.py` compiles every first-party source of a build directory (775 in `build/dev`)
with Homebrew `g++ -fsyntax-only`, about 2 min each run. Night of 2026-10-14: 29 sweeps, 68 min, 24% of
worker time; most units touched a handful of files. Pre-push hook still runs whole tree, so a per-unit
sweep of what changed loses nothing at landing.

## 2. What is built

`--changed` option. Same compiler, flags, output and exit status as today; only set of sources differs.

```
python3 tools/compiler_sweep.py --changed                     # since origin/develop
python3 tools/compiler_sweep.py --changed --base origin/main
```

- `--base <ref>` (default `origin/develop`), meaningful only with `--changed`.
- Changed files: `git diff --name-only --diff-filter=d $(git merge-base <base> HEAD)` (committed, staged,
  unstaged) plus untracked files not ignored (`git ls-files --others --exclude-standard`). Same set
  `quality_check.py` measures from.
- Source swept when its own file is changed, or when any header it read when last built is changed.
- Headers a source read: Ninja dependency log of build directory, `ninja -C <build-dir> -t deps`, matched to
  compile database entry by its `output` field. Paths in log relative to build directory; resolved before
  compare.

## 3. Rules that must hold

- Never fewer sources than needed: whenever what a source reads cannot be known, it is swept.
  - Entry whose object has no record in log, or record not `(VALID)`, swept.
  - `ninja` not on `PATH`, `ninja -t deps` fails, or compile database lacks `output` → whole tree swept, one
    line on stderr saying why.
  - Changed file named `CMakeLists.txt`, ending `.cmake`, or `CMakePresets.json` → whole tree swept (flags may
    have changed), line on stderr saying so.
- `git` failing (no repository, unknown base) → exit 2, as any sweep that cannot run.
- Nothing selected → exit 0, summary line says `0 of <n> sources`.
- Summary line with `--changed`: `compiler_sweep: <compiler>: <k> of <n> sources (changed since <base>),
  <w> warning(s), <x> not checked`. Without `--changed` unchanged.
- Without `--changed` behaviour byte for byte as today; pre-push hook keeps whole-tree sweep.

## 4. Files

| File | Change |
|---|---|
| `tools/compiler_sweep.py` | `--changed`, `--base`; `changed_files`, `ninja_deps`, `select_entries` |
| `tools/tests/test_compiler_sweep.py` | new, stdlib `unittest`, no compiler, no git repo needed for parsing tests |
| `tests/core/CMakeLists.txt` | `add_test(tools.compiler_sweep ...)` beside `tools.quality_check`, timeout 60 |
| `AGENTS.md` | Build and test: `--changed` in inner loop; whole tree before push (hook) |

## 5. Testing

- Parse of `ninja -t deps` text: record per object, `VALID` vs `STALE`, `#deps 0`.
- Selection: own file changed; header changed reaching source only through another header (log lists it);
  object with no record; stale record; CMake file changed → all; nothing changed → none.
- Fallbacks: deps command failing → all, reason given; entries without `output` → all.
- One test runs `main()` on fixture compile database and log with compiler stubbed to `true`: summary line
  shape.

## 6. Out of scope

- Changing pre-push hook or CI.
- Sweeping a source because a header it does not yet include was added.
- Speeding up whole-tree sweep.

## Open questions

None.
