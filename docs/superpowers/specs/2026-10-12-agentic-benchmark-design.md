# Agentic toolchain benchmark: design

Status: draft for owner. Nothing built. Task T055.

## 1. Problem

Toolchain changes often: `AGENTS.md`, skills (`picking-next-task`), plugins
(caveman, superpowers), model and effort, `tools/quality_check.py`, presets,
`tools/spec_router`. No number says whether a change helped. Judged by feel.

Want: after a change, one table. Same work, before and after: did it still
get done right, and what did it cost.

## 2. What "worked" means

Three questions, in this order. A change that fails an earlier one is not
saved by a later one.

1. Correct: task's check passes. Rules kept (section 5).
2. Cheap: points of weekly limit, tokens, wall time.
3. Clean: static-analysis findings, rework after landing.

A change worked only if correctness did not drop and median cost fell by more
than the spread between repeats of the unchanged toolchain.

## 3. Two instruments

### 3.1 Ledger (passive, free)

One line per finished task, from records that already exist. No extra runs.

| Field | Source |
|---|---|
| task id, size, points | task file Usage log (`pychron usage notes.md`) |
| tokens in / out / cache read / cache write, model, turns, tool calls by name, wall time | session log, `~/.claude/projects/<project>/<session>.jsonl` (`message.usage`, `tool_use` blocks, timestamps) |
| commits, files, lines | `git log --numstat` on task's branch |
| findings on changed lines | `python3 tools/quality_check.py --json` |
| CI result of first push containing it | `gh run list` |
| rework | later `fix` commit touching same lines within 14 days |
| toolchain version | `git rev-parse HEAD:AGENTS.md`, skill file hashes, plugin versions, model id |

Limits: task mix changes week to week, so ledger shows trend, not cause. Its
use: (a) points per session by size, which the budget already needs;
(b) finding where cost goes (builds, reading, retries); (c) noticing a
regression that the fixed set is too small to hold.

Kept outside repo, beside task file: `pychron agent ledger.jsonl`. Holds no
code and no prompt text, only counts and ids.

### 3.2 Fixed set (controlled)

Replay of tasks this repository has already done. For each task:

- start: parent of a real commit;
- prompt: the task as owner wrote it (task-file row), plus what an agent
  would have been told (interface names the hidden test needs);
- hidden check: tests the real commit added, copied in after agent stops,
  then built and run with the target's whole suite;
- then `quality_check.py --json` on agent's diff.

Run headless, one worktree per run, as `tools/spec_router` already does
(`claude -p --output-format json --model <m> --max-turns <n>`). Variant under
test = a directory holding the `AGENTS.md`, skills and settings to use;
baseline = `develop`'s.

Candidate tasks, all one component, each with its test in the same commit:

| Kind | Commit | What |
|---|---|---|
| UI fix, test first | `7a7e5352` | events list keeps newest 5000 |
| leak found by measurement | `cd1d31bd` | sim transport keeps newest 10000 frames |
| UI small feature | `b30f9ddb` | figure window title |
| config + canvas feature | `c336269d` | canvas background colour |
| concurrency defect | `9932e462` | job runner destructor waits |
| shipped files held by a test | `267660f9` | time zero in shipped plans |
| read-only investigation | T051 | graded against a written answer key |
| spec draft | T042 | graded by checklist: interfaces, rules, out of scope, compressed |

Hidden tests compile only if agent used reference's names. Rule: a task goes
in the set only if its test goes through an interface that exists at the
start commit, or the prompt can name the new interface in one line without
giving the solution away. `267660f9` and `c336269d` pass this; `9932e462`
passes (test uses existing `JobRunner` API).

Measures per run: pass, points (usage tool before and after; coarse, 1 %
steps), tokens by kind, wall time, turns, tool calls by name, builds started,
findings, lines changed against reference, rules kept.

## 4. Size of the experiment

Unknown until baseline is run once; that is step 1 of the plan.

- Smoke: 3 tasks, 1 run each. After any toolchain change. Catches "it no
  longer does the task". Says nothing about cost.
- Full: every task, 3 runs each, baseline and variant. Only for a change
  that claims a saving. 8 tasks x 3 x 2 = 48 runs; at an S each that is more
  than a week's budget, so full runs use the 6 code tasks on the cheapest
  model that passes baseline, or are spread over nights.
- Repeats exist because one run is noise. Report median and range, never a
  single run.

## 5. Rules kept (from `AGENTS.md`), read off the session log

- `quality_check.py` ran after last C++ edit and before tests.
- A test was written or changed before the source it tests.
- Target binary run filtered, not whole `ctest`, in inner loop.
- Commit subject matches Conventional Commits with a scope.
- No push, no merge, no `--no-verify`, no test disabled.
- Nothing sent outside the machine.

Each is a yes or no per run. A variant that saves tokens by skipping one is
not a saving.

## 6. Interfaces

    tools/agent_bench/tasks.toml      # id, start commit, prompt, hidden test paths, target, check command
    tools/agent_bench/run.py          # run <task|smoke|full> --variant <dir> --model <m> --repeats <n>
    tools/agent_bench/ledger.py       # session log + git + task file -> one ledger line
    tools/agent_bench/report.py       # ledger or runs -> table: baseline vs variant, median and range

Results: `build/agent_bench/<date>-<variant>/<task>-<n>.json`, never
committed. `tasks.toml` and answer keys are committed.

## 7. Out of scope

- Comparing models in general, or public benchmarks.
- Running in CI.
- Grading code style beyond `quality_check.py`.
- Judging a design: spec-draft task is graded by checklist only.
- Tasks needing owner's answer mid-run.

## 8. Order of work

1. `ledger.py`, run over the 22 existing session logs: where cost goes today.
2. `tasks.toml` with 3 smoke tasks, `run.py`, one baseline run of each.
   Gives real cost of a run; section 4 is rewritten from it.
3. Remaining tasks, `report.py`, baseline x 3.
4. First use: T054's `AGENTS.md` changes as the variant.

## 9. Questions for owner

- Weekly points that may go to benchmark runs?
- Headless runs acceptable unattended (they build and test, do not push)?
- Ledger beside task file, outside git: agreed?
- Smoke model: same as daily work, or cheapest that passes?
