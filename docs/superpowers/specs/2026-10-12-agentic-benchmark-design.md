# Agentic toolchain benchmark: design

Status: agreed 2026-10-12 (owner: "implement the agentic benchmark"). Built:
ledger, runner, report, three smoke tasks (T061). Section 10 says what is
built, what departs from the draft and what is left.

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

One line per session, from records that already exist. No extra runs. (Draft
said per task: a session log does not say where one task ends and the next
begins. Points per task stay in the task file's Usage log.)

| Field | Source |
|---|---|
| tokens in / out / cache read / cache write, model, turns, tool calls by name, wall time | session log, `~/.claude/projects/<project>/<session>.jsonl` (`message.usage`, `tool_use` blocks, timestamps) |
| what shell was used for (builds, ctest, test binaries, static analysis, commits, pushes) | `Bash` commands of the log, by pattern; command text not kept |
| rules kept (section 5) | order of edits and commands in the log |
| agents it started: count, turns, tokens | `<session>/subagents/*.jsonl` |

Not built: commits and lines from git, CI result of first push, rework, and
toolchain version per line. They need a session tied to a branch, which the
log gives only as the directory it ran in.

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

    tools/agent_bench/tasks.toml      # id, reference commit, prompt, hidden test paths, checks (argv lists)
    tools/agent_bench/run.py          # run <task|smoke|full> --label <l> --variant <commit|dir> --model <m> --repeats <n>
    tools/agent_bench/session_log.py  # one session log -> counts, shell use, rules kept
    tools/agent_bench/ledger.py       # every session of this checkout and its worktrees -> a line each, or a table
    tools/agent_bench/report.py       # one label, or variant against baseline: median, range, verdict

Results: `build/agent_bench/<label>/<task>-<n>.json`, never committed.
`tasks.toml` is committed.

`run.py --agent reference` applies the real commit in place of an agent and
`--agent none` applies nothing. A task is sound only if the first passes
its checks and the second fails them; both cost build time and no usage.
A check marked `before_hidden` runs before the real commit's tests are
copied in (static analysis of the agent's own lines).

Variant is laid over the start commit and committed before the agent
starts: every `AGENTS.md` and `CLAUDE.md` of a commit (default `HEAD`) or of
a directory. Agent's diff is counted from that commit.

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

## 9. Questions for owner, and what was built in their place

Not answered; each is an option, so no answer was needed to build.

- Weekly points for benchmark runs: nothing runs by itself. A person, or a
  scheduled task the owner writes, starts `run.py`.
- Headless runs unattended: `run.py` starts an agent with
  `--permission-mode acceptEdits` and tools `Bash Read Edit Write Glob Grep`
  in its own worktree. No agent run has been made yet: first one is the
  owner's to start or to allow.
- Ledger outside git: `ledger.py --out <file>` writes where it is told;
  default prints.
- Smoke model: `--model`; left out, the session's default.

## 10. As built (T061), and left

Built: section 3.1 as amended, 3.2 for three code tasks
(`sim-transport-bounded`, `job-runner-destructor`, `plan-time-zero`, all
`smoke`), section 6. Tests `tools/tests/test_agent_bench.py` (ctest
`tools.agent_bench`): no agent started, a repository made in a temporary
directory and a stand-in for `claude`.

Left:

- First baseline: `run.py smoke --label baseline`, then section 4 rewritten
  from what a run really costs.
- Points of weekly limit per run (usage tool before and after): runner
  records cost in dollars and tokens from the agent's own answer, not points.
- Three UI tasks of section 3.2 (`7a7e5352`, `b30f9ddb`, `c336269d`): their
  hidden tests name new interfaces; each needs its one-line hint and a
  `dev-ui` build, which is slow in a fresh worktree.
- Investigation and spec-draft tasks: need a grader (answer key, checklist).
- Section 5's rules read from a run's own session log are in each result
  (`session.rules`) but not in `report.py`'s table or verdict.
- Ledger fields listed as not built in 3.1.
- Rules are read from `Edit` and `Write` calls and from `Bash` command
  patterns. A file changed through the shell (a script, `sed`) is not seen as
  an edit, so "static analysis after last C++ edit" and "test before source"
  undercount: of 56 sessions only 8 show a C++ edit at all. Treat those two as
  a floor until edits made through the shell are recognised.
