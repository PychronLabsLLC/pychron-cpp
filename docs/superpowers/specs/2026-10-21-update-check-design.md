# Update check: design

Date 2026-10-21. Status: agreed in conversation, written for review.

## 1. What and why

Labs run stale versions; nothing tells them a release exists. Installation
wizard spec (`2026-10-03-installation-wizard-design.md`) left automatic
updates out of scope. This spec: tell user new release exists. Notify only:
nothing downloaded, verified, installed or restarted.

- pychron-ui: automatic check at startup, on by default, at most once per
  24 h, opt-out in Preferences. Manual *Help → Check for Updates…*.
- elctl: manual only, `elctl version --check`. No automatic check (runs in
  scripts, cron).
- Result shown quietly: status-bar indicator, never a dialog that opens by
  itself. Instrument may be mid-queue; nothing steals focus.

Source of truth: GitHub REST API,
`GET https://api.github.com/repos/PychronLabsLLC/pychron-cpp/releases/latest`.
Repository public; unauthenticated call works. Answers 200 with JSON, no
redirect, so existing `IHttpClient` (follows no redirect) is enough.
Download button opens release's `html_url` in browser; app fetches no asset.
Rate limit 60 unauthenticated requests per hour per IP; once a day per machine
far below it.

## 2. Components

### 2.1 `libs/core`: `pychron/core/http.hpp` (moved)

`HttpRequest`, `HttpResponse`, `IHttpClient` move from
`libs/earthbank/include/pychron/earthbank/http.hpp` to
`libs/core/include/pychron/core/http.hpp`, namespace `pychron::http`, bodies
unchanged. Contract comment moves with them: one request per call at most, no
retry, redirect not followed, any status is an answer, error means no answer
(`Timeout`, `Io`, `Config`, `kHttpNotSent`), no header value or body in an
error's `what`. `kHttpNotSent` moves too.

`libs/earthbank/.../http.hpp` keeps `using` aliases for moved names, its URL
and logging helpers, and `make_http_client()`. Earthbank code, `libs/earthbank_qt`
and `tests/earthbank` compile unchanged; their suite passing unchanged is the
proof. Rules of `libs/earthbank/AGENTS.md` apply to this change.

### 2.2 `libs/update` (new)

Qt-free, depends on `pychron_core` only. No I/O except through
`http::IHttpClient`. No clock read: `now` passed in.

```cpp
namespace pychron::update {

inline constexpr std::string_view kRepository = "PychronLabsLLC/pychron-cpp";

struct Version { int major = 0, minor = 0, patch = 0; auto operator<=>(const Version&) const = default; };

std::string to_string(Version version);  // "0.5.0", no leading "v"

struct Release {
  Version version;
  std::string tag, html_url, notes;  // notes: release body, markdown
};

// "v0.5.0" or "0.5.0". Pre-release or build suffix, missing part, sign,
// non-digit: error.
[[nodiscard]] Result<Version> parse_version(std::string_view text);

// Body of releases/latest. Draft, prerelease, missing tag_name or html_url,
// tag that parse_version refuses, malformed JSON: error. Missing or null body:
// empty notes. Unknown fields ignored.
[[nodiscard]] Result<Release> parse_latest_release(std::string_view json);

enum class FetchOutcome { Found, NoRelease };
struct Fetched { FetchOutcome outcome; std::optional<Release> release; };

// GET https://api.github.com/repos/<repository>/releases/latest,
// headers Accept: application/vnd.github+json and
// User-Agent: pychron-cpp/<PYCHRON_VERSION>, timeout 10 s.
// 200 + parses: Found. 404: NoRelease. Other status or unreadable 200:
// Protocol error ("GitHub answered <status>" for status). Client error:
// passed through.
[[nodiscard]] Result<Fetched> fetch_latest(http::IHttpClient& client,
                                           std::string_view repository = kRepository);

inline constexpr std::chrono::hours kCheckInterval{24};

struct CheckPolicy {
  bool enabled = true;
  std::optional<std::chrono::system_clock::time_point> last_check;
  std::optional<Version> skipped;
};

// enabled and (no last_check or now - last_check >= 24 h).
// last_check in the future (clock moved back): due.
[[nodiscard]] bool due(const CheckPolicy& policy, std::chrono::system_clock::time_point now);

// release when release.version > running and release.version != skipped;
// else nullopt. Skipped older than release does not hide it. `enabled`
// ignored: manual check reports whatever it finds.
[[nodiscard]] std::optional<Release> worth_showing(const Release& release, Version running,
                                                   const CheckPolicy& policy);
}
```

Repository is a constant, not a preference.

### 2.3 `apps/pychron-ui`

- `UpdateChecker`: owns one `std::thread` per check, started by
  `check_on_startup()` when `due()` and the client factory returns non-null, or
  by `check_now()`. Thread calls `fetch_latest`, posts `Result<Fetched>` to GUI
  thread with queued `QMetaObject::invokeMethod`. Constructed with
  `QSettings&`, running `Version`, `Now` and `ClientFactory`
  (`std::function`; null client = no Qt Network). Not on a simulated path:
  plain thread, no `Clock::Participant` (added to the real-time list of
  virtual-clock spec, sections 4.4 and 4.5). Destructor joins; quit during a
  check waits at most the 10 s timeout, answer never delivered. `IHttpClient`
  has no cancel; none added. Check asked for while one is out: not made.
- Signals: `available(const Release&, bool manual)`;
  `up_to_date(bool no_release, std::optional<Version> latest)` (manual only;
  `latest` older than running for dev build); `failed(const QString& reason,
  bool manual)`; `busy_changed(bool)`.
- `set_log(std::function<void(const QString&)>)`: UI has no logger object, so
  failed automatic check goes to this callback as `INFO [update] ...`
  (network, status) or `WARN [update] ...` (answer that does not read). Line
  window connects it to its log dock; data window connects none (failure
  silent there).
- `now` for `due()` and `last_check` from `CoreBridge::wall_now()` in line
  window; data window has no line, uses `SteadyClock::wall_now()`. Both
  windows get indicator, Help item and automatic check.
- Status-bar indicator "Update available: v<x.y.z>" when `worth_showing`
  returns a release (`UpdateIndicator`, `QToolButton`). Stays until quit or
  until that version skipped. Click opens update dialog.
- Update dialog: version, release notes (markdown), *Download* →
  `QDesktopServices::openUrl(html_url)`, *Skip this version* (stores
  `skipped`, hides indicator), *Close*.
- *Help → Check for Updates…*: ignores `due()` and `skipped`; always shows a
  dialog: up to date, update available (as above), or failure with reason.
  Runs on the same worker kind; menu item disabled while `busy()`. Action
  created and contributed to MenuHub Help menu (App scope) by
  `install_update_check(QMainWindow*, UpdateChecker&)` in `update_ui.cpp`, not
  by `brand.cpp`; same function adds indicator to status bar. One update
  dialog per window at a time: new one closes the one before.
- Preferences (QSettings, `apps/pychron-ui/AGENTS.md` recipe): page
  "Updates"; `Preferences::check_for_updates` is stored as
  `updates/check_on_startup` (not under `preferences/`). Keys:
  `updates/check_on_startup` (bool, default true; checkbox with tooltip saying
  what is sent and where), `updates/last_check` (ISO 8601 UTC),
  `updates/skipped_version` (string). Saved values untrusted: unparsable
  `last_check` → none; unparsable `skipped_version` → none.

### 2.4 `apps/elctl`

`elctl version` prints running version (as `--version`). `elctl version
--check` adds latest release and its URL. Exit status: 0 up to date (or no
release published, or this build newer), 10 newer release exists, 2 check
failed (reason on stderr), 2 also when build has no HTTP client (elctl built
without persistence, `PYCHRON_ELCTL_HAS_STORE` unset) and on usage error.
`--version` unchanged. `version_command(args, Io, const VersionDeps&)`:
`VersionDeps::make_client` injects client for tests; empty = earthbank's
`make_http_client()`.

## 3. Data flow

Automatic check, pychron-ui startup:

1. Load `CheckPolicy` from QSettings. If `due()` is false, stop.
2. If `make_http_client()` returns null (no Qt Network), stop.
3. Worker runs `fetch_latest`, posts result to GUI thread.
4. `last_check` is set to now only when the answer is `Found` or
   `NoRelease`. A failure leaves `last_check` alone, so an offline machine
   tries once per startup, never more.
5. If `worth_showing()` returns a release, show indicator.

Manual check (menu, `elctl`): skips steps 1 and 5's `skipped` rule; reports
every outcome. Also sets `last_check` on success (pychron-ui only).

## 4. Errors

| Case | Automatic | Manual |
|---|---|---|
| `Io`, `Timeout` | silent, one info log line | "Could not reach GitHub (<reason>)" |
| 403, 429, other non-200 but 404 | silent, info log with status | failure "GitHub answered <status>" |
| 404 | nothing shown | "No release published" (up to date) |
| bad JSON, missing field, draft, prerelease, bad tag | silent, warning log | failure "Unexpected answer from GitHub (<reason>)" |
| build without network client | nothing | failure "this build has no network client" |
| running version > latest (dev build) | nothing | up to date, both versions shown |

Never a crash, never a release shown from an answer that did not parse.

## 5. Rules that must hold

- Check reads nothing from and writes nothing to `Session`, executor, store
  or devices. A running queue is not touched.
- Request carries no machine id, lab name, user, path or config: only the
  headers in 2.2. User guide and Preferences tooltip say what is sent and to
  whom.
- No asset downloaded, no file written but QSettings keys in 2.3.
- No automatic check in `elctl`.
- `libs/update` has no Qt and no network code; `libs/core` gains no Qt.
- Thread code run once under `-DPYCHRON_SANITIZE=thread` before it lands.

## 6. Testing

- `tests/update/test_release.cpp`, `test_fetch.cpp`, `test_policy.cpp`
  (GoogleTest, no Qt, no network):
  - `parse_version`: with and without `v`; `0.10.0 > 0.9.0`; `1.0.0-rc.1`,
    `1.0`, `v`, `x.y.z`, `-1.0.0` refused.
  - `parse_latest_release`: fixture `tests/update/fixtures/latest.json`
    (captured from real API, trimmed); draft, prerelease, missing
    `tag_name`, missing `html_url`, malformed JSON.
  - `fetch_latest` with fake `IHttpClient`: 200, 404, 403, 500, `Io`,
    `Timeout`; request asserted: method `GET`, URL, both headers, timeout
    10 s.
  - `due`: disabled; never checked; 24 h minus 1 s; exactly 24 h; future
    `last_check`.
  - `worth_showing`: newer; equal; older; skipped equal to latest; skipped
    older than latest.
- `tests/earthbank`: unchanged, passes after move.
- `tests/ui`: Qt Test, not GoogleTest (one `QTEST_MAIN` binary per file,
  `tests/ui/CMakeLists.txt`): `test_update_checker.cpp` (policy round trip of
  three keys; garbage `last_check`/`skipped_version` → none; checker against
  fake client), `test_update_ui.cpp` (indicator appears for newer release,
  hides after skip; dialogs), `test_preferences.cpp` (checkbox round trip).
  Fakes: `tests/update/fake_http.hpp` via INTERFACE target
  `pychron::update_testing`, which elctl and UI tests link.
- elctl: `apps/elctl/tests/test_version_cmd.cpp`, `version --check` exit 0, 10,
  2 with injected fake client.

## 7. Out of scope

- Download, checksum or signature verification, install, restart (own spec
  later; needs checksums and signatures on release assets first, Windows
  installer is unsigned).
- Checksums or signatures added to releases.
- Fleet pinning, channels, prereleases.
- Automatic check in `elctl`.
- Cancel on `IHttpClient`.
- Package repository (apt) for Linux.

## 8. Also in this branch

`AGENTS.md` said repository private; `gh repo view` says public. Line
corrected: branch protection needs paid plan only for a private repository,
so reason given there was stale; it now also says repository stays public,
since anonymous check in this spec depends on it.

## Open questions

None.
