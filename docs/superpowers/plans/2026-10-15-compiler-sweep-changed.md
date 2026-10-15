# compiler_sweep.py --changed Implementation Plan

Spec: `docs/superpowers/specs/2026-10-15-compiler-sweep-changed-design.md`.

### Task 1: Selection, tested

Files: `tools/compiler_sweep.py`, `tools/tests/test_compiler_sweep.py`, `tests/core/CMakeLists.txt`.

- [x] Write `tools/tests/test_compiler_sweep.py`: parsing of `ninja -t deps` output (valid, stale, no deps),
  selection by own file and by header, missing record, stale record, CMake file changed, nothing changed,
  failing deps command, entries without `output`. See them fail.
- [x] Add `parse_ninja_deps`, `ninja_deps`, `changed_files`, `select_entries` to `tools/compiler_sweep.py`.
- [x] Register `tools.compiler_sweep` in `tests/core/CMakeLists.txt` beside `tools.quality_check`.
- [x] Tests pass.

### Task 2: The option

Files: `tools/compiler_sweep.py`, `tools/tests/test_compiler_sweep.py`.

- [x] Test: `main()` with `--changed` on a fixture database, stubbed compiler and stubbed git and deps,
  prints `<k> of <n> sources (changed since <base>)`; without `--changed` the summary is as before.
- [x] Add `--changed` and `--base`; whole-tree fallbacks print their reason on stderr; git failure exits 2.
- [ ] Run `python3 tools/compiler_sweep.py --changed` on a real branch and compare its count with a full
  sweep's.

### Task 3: Docs

Files: `AGENTS.md`.

- [ ] Build and test section: `--changed` for the inner loop; the whole tree is what the pre-push hook runs.

## Open questions

None.
