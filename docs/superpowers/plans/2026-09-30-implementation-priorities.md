# pychron-cpp implementation priorities

Date: 2026-09-30
Status: Draft
Owner: Jake Ross

Prioritized backlog for the requested pychron-cpp capabilities, with the
library decisions each depends on.

## Ranking drivers

1. **Agent-driven development with no guard rails.** Most work lands through
   agents (worktrees, `unit/*` merges, `.spec_router`). CI and an AGENTS.md
   protect everything else, so they go first.
2. **Core is Qt-free.** `libs/*` is built on asio with a Qt-free boundary
   (instrument-control design, section 10). Qt-based libraries are acceptable
   only inside `apps/pychron-ui`.
3. **Dependencies.** Sample/irradiation persistence waits on ADR-0002
   (`2026-09-29-persistence-adr.md`). The installation wizard waits on the
   config schema settling (see `2026-09-30-legacy-config-survey.md`).

## Priorities

| # | Item | Value | Cost | Status | Notes |
|---|---|---|---|---|---|
| 1 | CI/CD | High | S | In review (PR #1) | GitHub Actions: macOS clang + ASan/UBSan, Linux gcc-14, Linux clang-18 + ASan/UBSan, Windows MSVC, Qt6 UI with `tests/ui` headless. Follow-ups: clang-format/clang-tidy checks, release artifacts. |
| 2 | AGENTS.md | High | S | Not started | Build presets, library layering (core <- transport <- codecs <- devices <- systems), Qt-free core rule, GPLv3 licensing rule, spec locations, test expectations, branch/worktree workflow. Reference from CLAUDE.md. The legacy pychron repo's AGENTS.md is a starting point. |
| 3 | Legacy functionality spreadsheet | High | M | Not started | Survey `pychron/pychron/*` (96 packages). Columns: package, feature, user-facing, used in lab, C++ status (done / spec'd / none), **deprecate/remove flag**, notes. Delivered as .xlsx; flags are set by the owner. Scopes all later work. |
| 4 | Robust logging | High | M | Not started | `Logger` currently publishes to the SignalBus and echoes to one stream; spdlog is already a vcpkg dependency but unused. Add rotating file sinks, per-logger levels from TOML (e.g. `transport.serial.*=trace`), route `TraceRecorder` bytes to the debug log, flush on fatal/crash, filtering in the UI log dock. |
| 5 | RPC service | High | L | Not started | Other pychrons must query and actuate valves. Speak the legacy `pychron/tx` wire protocol first so existing Python pychrons can drive the C++ one during migration. New `libs/rpc` on asio; commands dispatch to `SwitchManager` / `ExtractionLine` via the scheduler. Step 1: short spec of the legacy command set (GetValveState, Open, Close, ...). Security: bind to localhost or an allowlist; read-only by default; actuation opt-in. |
| 6 | Plotting (Hardware-plugin style) | Med-High | M | Partly done | Core ring and spectrometer strip chart delivered (see `2026-10-01-spectrometer-window-design.md`); gauge plot dock remains (ring fed by the gauge scanner, QCustomPlot dock in the UI). |
| 7 | Preferences window | Medium | M | Not started | Typed preferences model in core first (TOML, schema, defaults, overrides per instrument-control design section 5.4); UI dialog generated from the schema. The same model feeds the wizard. |
| 8 | Sample/irradiation persistence | High (long term) | L | Blocked | Needs ADR-0002 action items 1-2 (accept ADR, write schema spec). Then catalog tables: sample, project, PI, material, irradiation, level, position. |
| 9 | Installation wizard | Medium | M | Blocked | Config schema still moving. Start as `elctl init` (scaffold config, device check); Qt wizard later. |

## Library decisions

| Library | Decision | Reason |
|---|---|---|
| QCustomPlot | **Accepted** for `apps/pychron-ui` | Project licensed GPLv3, so QCustomPlot's GPL terms are compatible. |
| TinyORM | Not recommended | Depends on QtSql, which cannot enter the Qt-free core; persistence must also run headless under `elctl`. Prefer sqlite3 + libpq behind a thin repository layer, or sqlpp11 for type-safe queries. Settle in the persistence schema spec. |
| QSerialPort | Rejected | Core is Qt-free; transport already uses asio `serial_port`. |
| libmodbus | Reference and test oracle only | It owns its socket I/O, which conflicts with the per-transport queue, retries and health, `TraceRecorder`, and `SimTransport` replay (instrument-control design, section 4.2). Hand-roll RTU/TCP framing (~300 lines) as a codec. |

## Licensing

- Project license: GPLv3 (`LICENSE`).
- Open: declare `GPL-3.0-only` or `GPL-3.0-or-later` (recommended: or-later)
  via SPDX tags and a README note.
- Code ported from legacy pychron (Apache-2.0) is compatible; keep its
  original notices on ported files.

## Sequence

1. Now: CI (PR #1), AGENTS.md, legacy functionality survey in background.
2. Next: logging, then RPC spec and implementation.
3. In parallel: accept ADR-0002 and write the persistence schema spec.
4. Later: preferences model, plotting, installation wizard, persistence.
