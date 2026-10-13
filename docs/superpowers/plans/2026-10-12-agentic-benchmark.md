# Agent toolchain benchmark: plan

Spec: `docs/superpowers/specs/2026-10-12-agentic-benchmark-design.md`. Task
T061. Tasks 1 to 3 were built together and are one commit; every test starts
no agent.

## Task 1: the session log reader and the ledger (done)

- Files: `tools/agent_bench/session_log.py`, `tools/agent_bench/ledger.py`,
  `tools/tests/test_agent_bench.py` (`SessionLog`, `Ledger`),
  `tests/core/CMakeLists.txt` (ctest `tools.agent_bench`).
- Done when: `python3 tools/agent_bench/ledger.py --summary` prints a row
  for each session of this checkout and its worktrees, a reply written as
  several log lines is counted once, and no command text or prompt reaches
  the output.

## Task 2: the runner and three smoke tasks (done)

- Files: `tools/agent_bench/run.py`, `tools/agent_bench/tasks.toml`,
  `tools/tests/test_agent_bench.py` (`Runner`).
- Done when: on a two-commit repository made by the test, a stand-in agent
  that fixes the fault passes, one that cannot be started is a failed run,
  the guide under test is the variant's, the agent's diff leaves out the
  guide and the hidden test, and the worktree is gone afterwards; and on
  this repository `run.py smoke --agent reference` passes all three tasks
  and `--agent none` fails all three.

## Task 3: the report (done)

- Files: `tools/agent_bench/report.py`, `tools/tests/test_agent_bench.py`
  (`Report`).
- Done when: a label prints pass counts with median and range, and two
  labels print a verdict per task that puts pass rate before cost and
  counts a saving only past the baseline's own range.

## Task 4: the first baseline (the owner's to start)

- `python3 tools/agent_bench/run.py smoke --label baseline`: three agents,
  one after another, each in its own worktree with a fresh `dev` build.
- Then: rewrite the spec's section 4 from what the runs cost, and decide
  repeats and model for full runs.

## Task 5: the remaining tasks of the set

- The three UI tasks (a `dev-ui` build each), then a grader for the
  investigation and spec-draft tasks. Each new task is checked with
  `--agent reference` and `--agent none` before it is committed.

## Task 6: first use

- A change to the guide as a variant against the baseline: `run.py full
  --label <name> --variant <commit or directory> --repeats 3`, then
  `report.py baseline <name>`.
