# Agent guide

## Workflow

- Single developer. Do not open pull requests. Commit on a branch (or worktree),
  then merge into `main` and push.
- Before pushing, bring `main` up to date with `origin/main` (rebase local work
  on top of it) and run the tests.
- Never skip or disable a failing test; find the root cause.

## Build and test

See `docs/dev_setup.md` for setup and `CMakePresets.json` for presets (CI uses
`dev`). Tests are GoogleTest, one `test_<component>.cpp` per component under
`tests/`, run with `ctest`.

- CI (`.github/workflows/ci.yml`) builds with clang + ASan/UBSan on macOS,
  gcc 14 and clang 18 + ASan/UBSan on Ubuntu 24.04, and MSVC.
- `-DPYCHRON_SANITIZE=address,undefined` enables ASan/UBSan.
- gcc 13 (the Ubuntu 24.04 default `g++`) warns where the CI compilers do not;
  build it with `-DPYCHRON_WARNINGS_AS_ERRORS=OFF`.
- `-DPYCHRON_SCRIPTING=OFF` drops the embedded CPython dependency.
- Ubuntu 24.04's cmake 3.28 is too old for this tree (`pip install cmake`).

Compilers disagree about undefined behaviour: a test that passes under clang
can abort under gcc, and the reverse. A failure on one compiler only is a real
bug until shown otherwise.

## Lifetime rules

Systems components take non-owning references (`Scheduler&`, `SignalBus&`,
`Clock&`, role pointers such as `IIntensityAcquirer*`). Whatever they point at
must outlive them. In tests, declare the fakes before the fixture that owns the
component: locals are destroyed in reverse order of declaration.
