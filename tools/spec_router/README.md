# spec-router

Breaks the design spec into work units and delegates each unit to a headless
Claude Code agent, gated by build + test results.

Division of labor:

- **Code owns the rules.** Units, layers, dependencies and waves live in
  `spec_router/units.toml` (mirrors spec section 9.4). Markdown sectioning,
  worktree management, `claude -p` invocation, `cmake`/`ctest`, and merging
  are plain code.
- **Jev (TypeSafe System One) owns the semantic mapping.** Per spec section:
  which unit it primarily specifies (Choice) and which other units must read
  it (one Noul per unit). Per unit: how large the job is (Score) -> model tier
  and turn budget. Per agent report: outcome / evidence / in-scope judgments
  that gate the merge together with the router's own ctest run.

All judgments are cached in `.spec_router/judgments.json` keyed by content
hash, so re-planning is free unless the spec or units change.

## Setup

```bash
cd tools/spec_router
uv venv .venv && uv pip install -p .venv/bin/python -e '.[dev]'
export TYPESAFE_API_KEY=...        # Jev
npm i -g @anthropic-ai/claude-code # `claude` on PATH, or pass --claude-bin
```

## Use (from the repo root)

```bash
tools/spec_router/.venv/bin/spec-router plan            # route + size, prints table
tools/spec_router/.venv/bin/spec-router prompt core     # inspect an agent prompt
tools/spec_router/.venv/bin/spec-router run --wave 1 --dry-run
tools/spec_router/.venv/bin/spec-router run --wave 1    # live: agent -> build -> ctest -> judge -> merge
tools/spec_router/.venv/bin/spec-router status
```

`run` executes waves in order; units in the same wave run in parallel, each in
`.worktrees/<unit>` on branch `unit/<unit>`. A unit is merged (`--no-ff` into
the current branch) only when the router's own configure/build/ctest is green
and Jev judges the agent's report `complete` and in scope. Anything else stops
the run with the branch left in place for inspection.

Flags: `--unit <id>` runs one unit; `--no-merge` verifies without merging;
`--force` overrides the dependency and split guards; `--parallel N`.

## Tests

```bash
tools/spec_router/.venv/bin/python -m pytest -q tools/spec_router
```

No test touches the network or spawns agents.
