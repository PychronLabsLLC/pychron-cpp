# Update Check Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Tell the user, in pychron-ui and through `elctl version --check`, that a newer release than the running one is published on GitHub.

**Architecture:** The HTTP seam (`IHttpClient`) moves from `libs/earthbank` to `libs/core`. A new Qt-free library, `libs/update`, parses the GitHub "latest release" answer, compares versions and decides when a check is due, talking to the network only through that seam. pychron-ui runs the check on a worker thread at startup and shows a status-bar indicator; elctl runs it on demand.

**Tech Stack:** C++20, nlohmann_json, GoogleTest, Qt 6 Widgets (UI only), the existing `libs/earthbank_qt` HTTPS client.

**Spec:** `docs/superpowers/specs/2026-10-21-update-check-design.md`

## Global Constraints

- URL: `https://api.github.com/repos/PychronLabsLLC/pychron-cpp/releases/latest`; repository is the constant `kRepository`, never a preference.
- Request headers exactly: `Accept: application/vnd.github+json`, `User-Agent: pychron-cpp/<PROJECT_VERSION>`. Nothing else identifying is sent. Timeout 10 s.
- Automatic check at most once per 24 h, pychron-ui only, on by default. `elctl` never checks on its own.
- `last_check` is written only on `Found` or `NoRelease`, never on a failure.
- No asset downloaded, no file written except the QSettings keys `updates/check_on_startup`, `updates/last_check` (ISO 8601 UTC), `updates/skipped_version`.
- `libs/update` has no Qt and no network code; `libs/core` gains no Qt.
- The check never reads or writes `Session`, executor, store or devices; the result never opens a dialog by itself.
- Timestamps from the clock (`CoreBridge::wall_now()`), never `QDateTime::currentDateTime()`.
- elctl exit status: 0 up to date or no release, 10 newer release, 2 failure or no HTTP client in the build.
- Static analysis (`python3 tools/quality_check.py`) and `tools/compiler_sweep.py --changed` after each C++ task; the gcc/clang/MSVC pitfalls listed in `AGENTS.md` apply (no `const auto [a, b]` over pairs, no unbraced `if` around `EXPECT_*`, no dropped `json::parse` result, no raw string literal inside a gtest macro).

## Review Focus

1. A tag with a leading `v` and a two-digit part (`v0.10.0` against running `0.9.3`) must show as newer: string comparison would say otherwise. Test in Task 2.
2. A machine whose clock was set back (saved `last_check` in the future) must still check, not wait until the clock catches up. Test in Task 4.
3. Quitting pychron-ui while a check is in flight must neither crash nor deliver the result to a destroyed object. Test in Task 6.
4. A skipped version must not hide a still-newer release published after it (skipped 0.5.0, latest 0.6.0 shows). Test in Task 4.
5. A build without Qt Network (`make_http_client()` null) must start pychron-ui with no check and no error, and `elctl version --check` must say why and exit 2. Tests in Tasks 5 and 6.

---

### Task 1: Move the HTTP seam to `libs/core`

**Files:**
- Create: `libs/core/include/pychron/core/http.hpp`
- Modify: `libs/earthbank/include/pychron/earthbank/http.hpp` (the `HttpRequest`/`HttpResponse`/`IHttpClient`/`kHttpNotSent` block and its contract comment, roughly lines 25–83)
- Test: `tests/core/test_http.cpp` (add to `pychron_core_tests` sources in `tests/core/CMakeLists.txt`)

**Interfaces:**
- Produces: namespace `pychron::http` with `HttpRequest`, `HttpResponse`, `IHttpClient` (`virtual Result<HttpResponse> send(const HttpRequest&) = 0`), `inline constexpr std::string_view kHttpNotSent = "http_not_sent"`. `pychron::earthbank` keeps `using HttpRequest = http::HttpRequest;` (and the other three) so every existing name still resolves. `request_may_have_arrived`, `url_encode`, `with_query`, `url_path`, `find_header`, `request_line` and `make_http_client()` stay in earthbank, unchanged.

- [ ] **Step 1: Write the failing tests**
  - In `tests/earthbank/test_http.cpp` (it already includes earthbank's `http.hpp`): `static_assert(std::is_same_v<pychron::earthbank::IHttpClient, pychron::http::IHttpClient>);` and the same for `HttpRequest` and `HttpResponse`.
  - In `tests/core/test_http.cpp`, suite `Http`:
  - `Http.RequestDefaultsToThirtySeconds`: `EXPECT_EQ(pychron::http::HttpRequest{}.timeout, std::chrono::milliseconds{30000});`
  - `Http.NotSentCode`: `EXPECT_EQ(pychron::http::kHttpNotSent, "http_not_sent");`
- [ ] **Step 2: Build and see it fail**
  Run: `cmake --build --preset dev --target pychron_core_tests`
  Expected: compile error, `pychron/core/http.hpp` not found.
- [ ] **Step 3: Move the declarations** into `libs/core/include/pychron/core/http.hpp` (`#pragma once`, namespace `pychron::http`, contract comment carried over word for word except "this library"/"the service" wording that is EarthBank's: the core header says what any client promises; the sentence on why a POST must not be repeated stays in earthbank's header beside the aliases). Replace them in earthbank's header with the four `using` declarations and `#include "pychron/core/http.hpp"`.
- [ ] **Step 4: Run the tests**
  Run: `cmake --build --preset dev --target pychron_core_tests pychron_earthbank_tests pychron_elctl_tests && build/dev/tests/core/pychron_core_tests --gtest_filter='Http.*' && build/dev/tests/earthbank/pychron_earthbank_tests && build/dev/apps/elctl/tests/pychron_elctl_tests --gtest_filter='*Earthbank*'`
  Expected: all pass, no earthbank or elctl source changed beyond the header.
- [ ] **Step 5: Static analysis**, `python3 tools/quality_check.py` exit 0; `python3 tools/compiler_sweep.py --changed` clean.
- [ ] **Step 6: Commit** `refactor(core): the HTTP client seam lives in core`

### Task 2: `libs/update`: versions and release parsing

**Files:**
- Create: `libs/update/CMakeLists.txt`, `libs/update/include/pychron/update/update.hpp`, `libs/update/src/release.cpp`
- Create: `tests/update/CMakeLists.txt`, `tests/update/test_release.cpp`, `tests/update/fixtures/latest.json`
- Modify: `CMakeLists.txt` (add `update` to `PYCHRON_LIBS` right after `core`)

**Interfaces:**
- Consumes: `pychron::Result`, `pychron::Error` (`libs/core` `error.hpp`).
- Produces (all in `pychron::update`, header `pychron/update/update.hpp`):
  - `inline constexpr std::string_view kRepository = "PychronLabsLLC/pychron-cpp";`
  - `struct Version { int major = 0, minor = 0, patch = 0; auto operator<=>(const Version&) const = default; };`
  - `std::string to_string(Version)` → `"0.5.0"` (no `v`).
  - `struct Release { Version version; std::string tag, html_url, notes; };`
  - `[[nodiscard]] Result<Version> parse_version(std::string_view text);`
  - `[[nodiscard]] Result<Release> parse_latest_release(std::string_view json);`

CMake: `add_library(pychron_update STATIC ...)`, alias `pychron::update`, `PUBLIC pychron::core`, `PRIVATE nlohmann_json::nlohmann_json`, `target_compile_definitions(pychron_update PRIVATE PYCHRON_UPDATE_USER_AGENT="pychron-cpp/${PROJECT_VERSION}")` (used in Task 3), `pychron_set_warnings`. A header comment in the style of `libs/earthbank/CMakeLists.txt` saying what the library is and that it has no Qt and no network code. Test CMake as `tests/earthbank/CMakeLists.txt`: glob `test_*.cpp`, `PYCHRON_UPDATE_FIXTURE_DIR` definition, `gtest_discover_tests(... PROPERTIES TIMEOUT 60)`.

Fixture: `gh api repos/PychronLabsLLC/pychron-cpp/releases/latest`, trimmed to `tag_name`, `name`, `html_url`, `draft`, `prerelease`, `body`, `published_at`, and one entry of `assets` (to show unknown fields are ignored).

- [ ] **Step 1: Write the failing tests** in `tests/update/test_release.cpp`, suite `UpdateVersion` and `UpdateRelease`:
  - `UpdateVersion.AcceptsWithAndWithoutV`: `parse_version("v0.5.0")` and `parse_version("0.5.0")` both `Version{0,5,0}`.
  - `UpdateVersion.ComparesNumerically`: `*parse_version("v0.10.0") > *parse_version("0.9.3")`.
  - `UpdateVersion.RefusesOtherText`: each of `"1.0.0-rc.1"`, `"1.0.0+build"`, `"1.0"`, `"v"`, `""`, `"x.y.z"`, `"-1.0.0"`, `"1.0.0 "`, `"01.0.0x"` returns an error.
  - `UpdateVersion.ToString`: `to_string(Version{1,2,3}) == "1.2.3"`.
  - `UpdateRelease.ReadsTheFixture`: fields of `fixtures/latest.json` (version from its `tag_name`, `html_url`, `notes` == its `body`).
  - `UpdateRelease.RefusesDraft`, `RefusesPrerelease` (fixture with the flag flipped), `RefusesMissingTag`, `RefusesMissingUrl`, `RefusesBadTag` (`"tag_name": "nightly"`), `RefusesMalformedJson` (`"{"`), `RefusesNonObject` (`"[]"`).
  - `UpdateRelease.MissingBodyIsEmptyNotes`: no `body` or `"body": null` → `notes` empty, still parses.
- [ ] **Step 2: Build and see it fail**
  Run: `cmake --preset dev && cmake --build --preset dev --target pychron_update_tests`
  Expected: compile errors (functions not defined).
- [ ] **Step 3: Implement** `parse_version` (optional single leading `v`, then exactly three dot-separated runs of ASCII digits, nothing after; `std::from_chars` per part, overflow is an error) and `parse_latest_release` (`json::parse(text, nullptr, false)`, keep the result, `is_discarded()` → error; `ErrorKind` of each error is `Parse` or the kind `error.hpp` uses for malformed input elsewhere, with a message naming the field).
- [ ] **Step 4: Run the tests**
  Run: `build/dev/tests/update/pychron_update_tests --gtest_filter='UpdateVersion.*:UpdateRelease.*'`
  Expected: PASS.
- [ ] **Step 5: Static analysis** as Task 1.
- [ ] **Step 6: Commit** `feat(update): parse GitHub's latest release and compare versions`

### Task 3: `fetch_latest`

**Files:**
- Create: `libs/update/src/fetch.cpp`, `tests/update/fake_http.hpp`, `tests/update/test_fetch.cpp`
- Modify: `libs/update/include/pychron/update/update.hpp`

**Interfaces:**
- Consumes: `pychron::http::IHttpClient` (Task 1), `parse_latest_release` (Task 2).
- Produces:
  - `enum class FetchOutcome { Found, NoRelease };`
  - `struct Fetched { FetchOutcome outcome; std::optional<Release> release; };` (`release` set only for `Found`)
  - `[[nodiscard]] Result<Fetched> fetch_latest(http::IHttpClient& client, std::string_view repository = kRepository);`
  - `tests/update/fake_http.hpp`: `pychron::update::testing::FakeHttp final : http::IHttpClient`, header-only, thread-safe (its own `std::mutex`), with:
    - `std::vector<http::HttpRequest> sent() const;`
    - a queue of answers: `void answer(int status, std::string body);`, `void fail(ErrorKind kind);` (`send` with an empty queue fails the test with `ADD_FAILURE()` and returns an `Io` error);
    - `void hold();` / `void release();`: while held, `send` blocks until `release()`;
    - `std::unique_ptr<http::IHttpClient> borrow();`: a forwarding client that calls this fake, for factories that must hand out ownership.
    Tasks 5, 6 and 7 include it (they add `tests/update` to their include path). Answer bodies are built in tests from `fixtures/latest.json` with `tag_name` replaced: helper `std::string release_json(std::string_view tag)` in the same header.

- [ ] **Step 1: Write the failing tests** `tests/update/test_fetch.cpp`, suite `UpdateFetch`:
  - `AsksTheRightThing`: one request; `method == "GET"`; `url == "https://api.github.com/repos/PychronLabsLLC/pychron-cpp/releases/latest"`; `find_header`-style lookup finds `Accept: application/vnd.github+json` and `User-Agent` starting `pychron-cpp/`; exactly those two headers; `timeout == 10s`; empty body.
  - `OtherRepository`: `fetch_latest(fake, "a/b")` asks `.../repos/a/b/releases/latest`.
  - `FoundOn200`: fixture body → `Found`, release version as fixture.
  - `NoReleaseOn404`: → `NoRelease`, `release` empty.
  - `FailsOnOtherStatus`: 403, 429, 500 each an error whose message contains the status number.
  - `FailsOnUnparsable200`: body `"{"` → error.
  - `PassesClientErrorsThrough`: `Io` → error of kind `Io`; `Timeout` → `Timeout`.
- [ ] **Step 2: Run, see it fail** (`cmake --build --preset dev --target pychron_update_tests`; compile error).
- [ ] **Step 3: Implement** `fetch_latest` in `src/fetch.cpp`; User-Agent from `PYCHRON_UPDATE_USER_AGENT`.
- [ ] **Step 4: Run** `build/dev/tests/update/pychron_update_tests --gtest_filter='UpdateFetch.*'`, expected PASS.
- [ ] **Step 5: Static analysis.**
- [ ] **Step 6: Commit** `feat(update): fetch the latest release through the HTTP seam`

### Task 4: Check policy

**Files:**
- Create: `libs/update/src/policy.cpp`, `tests/update/test_policy.cpp`
- Modify: `libs/update/include/pychron/update/update.hpp`

**Interfaces:**
- Produces:
  - `struct CheckPolicy { bool enabled = true; std::optional<std::chrono::system_clock::time_point> last_check; std::optional<Version> skipped; };`
  - `inline constexpr std::chrono::hours kCheckInterval{24};`
  - `[[nodiscard]] bool due(const CheckPolicy& policy, std::chrono::system_clock::time_point now);`
  - `[[nodiscard]] std::optional<Release> worth_showing(const Release& release, Version running, const CheckPolicy& policy);`

- [ ] **Step 1: Write the failing tests** `tests/update/test_policy.cpp`, suites `UpdateDue`, `UpdateWorthShowing` (fixed `now`, no clock read):
  - `UpdateDue.DisabledNeverDue` (even with no `last_check`).
  - `UpdateDue.NeverCheckedIsDue`.
  - `UpdateDue.JustUnderADayIsNot`: `last_check = now - 24h + 1s` → false.
  - `UpdateDue.ExactlyADayIs`: `now - 24h` → true.
  - `UpdateDue.FutureLastCheckIsDue`: `last_check = now + 1h` → true (Review Focus 2).
  - `UpdateWorthShowing.Newer`: running 0.4.0, release 0.5.0 → release.
  - `UpdateWorthShowing.EqualOrOlder`: release 0.4.0 and 0.3.9 against running 0.4.0 → nullopt.
  - `UpdateWorthShowing.SkippedIsHidden`: skipped 0.5.0, release 0.5.0 → nullopt.
  - `UpdateWorthShowing.SkippedOlderDoesNotHide`: skipped 0.5.0, release 0.6.0 → release (Review Focus 4).
  - `UpdateWorthShowing.DisabledStillAnswers`: `enabled = false` does not change the answer (enabled governs `due` only; a manual check reports whatever it finds).
- [ ] **Step 2: Run, see it fail.**
- [ ] **Step 3: Implement** both functions in `src/policy.cpp`.
- [ ] **Step 4: Run** `--gtest_filter='UpdateDue.*:UpdateWorthShowing.*'`, expected PASS.
- [ ] **Step 5: Static analysis.**
- [ ] **Step 6: Commit** `feat(update): when to check and what is worth showing`

### Task 5: `elctl version [--check]`

**Files:**
- Create: `apps/elctl/src/version_cmd.hpp`, `apps/elctl/src/version_cmd.cpp`, `apps/elctl/tests/test_version_cmd.cpp`
- Modify: `apps/elctl/src/cli.cpp` (dispatch `version` beside `doctor`, before any line is opened; usage text near line 60), `apps/elctl/src/cli.hpp` (add `kUpdateAvailable = 10` to `ExitCode`), `apps/elctl/CMakeLists.txt` (link `pychron::update`; tests get `tests/update` on the include path), `docs/user/08-elctl-reference.md`

**Interfaces:**
- Consumes: `fetch_latest`, `parse_version` (running version from `pychron::setup::version()`). Not `worth_showing`: a manual check has no skipped version, so it compares `release.version` with the running one directly. `pychron::earthbank::make_http_client()` for the real client.
- Produces, in namespace `elctl`:
  - `struct VersionDeps { std::function<std::unique_ptr<pychron::http::IHttpClient>()> make_client; };` — empty: `pychron::earthbank::make_http_client`.
  - `int version_command(const std::vector<std::string>& args, Io io, VersionDeps deps = {});`

Output (stdout), one item per line:
- always `elctl <version>`;
- with `--check`, up to date: `latest: <x.y.z> (up to date)`; no release: `latest: none published`; newer: `latest: <x.y.z>` then `download: <html_url>`; running newer than latest: `latest: <x.y.z> (this build is newer)` and exit 0.
- failure: `elctl version: could not check for updates: <reason>` on stderr, exit 2. No client in the build: reason `this build has no network client`.
- unknown argument: usage on stderr, exit 2.

- [ ] **Step 1: Write the failing tests** `apps/elctl/tests/test_version_cmd.cpp`, suite `VersionCmd`, with `FakeHttp` handed in as `VersionDeps{[&] { return fake.borrow(); }}`:
  - `PlainPrintsVersionAndSendsNothing`: `{}` → exit 0, stdout starts `elctl `, `fake.sent` empty.
  - `UpToDate`: latest equals running (build the fixture body with `tag_name` = `v` + running version) → exit 0, `"(up to date)"`.
  - `Newer`: tag `v999.0.0` → exit 10, stdout contains `latest: 999.0.0` and the fixture's `html_url`.
  - `NoRelease`: 404 → exit 0, `none published`.
  - `BuildIsNewer`: tag `v0.0.1` → exit 0, `this build is newer`.
  - `Failure`: `Io` → exit 2, stderr contains `could not check for updates`.
  - `UnknownArgument`: `{"--chek"}` → exit 2.
  - `NoClient` (Review Focus 5): `VersionDeps{[] { return std::unique_ptr<pychron::http::IHttpClient>{}; }}` → exit 2, stderr contains `this build has no network client`.
- [ ] **Step 2: Run, see it fail** (`cmake --build --preset dev --target pychron_elctl_tests`).
- [ ] **Step 3: Implement** `version_command`; wire `if (command == "version") return version_command(rest, io);` in `run()`; add `elctl version [--check]` to the usage text; keep `--version` as is.
- [ ] **Step 4: Run** `build/dev/apps/elctl/tests/pychron_elctl_tests --gtest_filter='VersionCmd.*'`, expected PASS.
- [ ] **Step 5: Document** `elctl version [--check]`, its output and exit codes 0/10/2, and exactly what is sent, in `docs/user/08-elctl-reference.md`.
- [ ] **Step 6: Static analysis.**
- [ ] **Step 7: Commit** `feat(elctl): version --check reports a newer release`

### Task 6: pychron-ui check engine (settings and worker)

**Files:**
- Create: `apps/pychron-ui/src/update_checker.hpp`, `apps/pychron-ui/src/update_checker.cpp`, `tests/ui/test_update_checker.cpp`
- Modify: `apps/pychron-ui/CMakeLists.txt` (sources, link `pychron::update`), `tests/ui/CMakeLists.txt` (include `tests/update`), `docs/superpowers/specs/2026-10-06-virtual-clock-design.md` section 4.4 (add the update check's worker and its request timeout to the real-time list)

**Interfaces:**
- Consumes: `fetch_latest`, `due`, `worth_showing`, `CheckPolicy`, `parse_version`, `to_string` (Tasks 2–4).
- Produces (namespace `pychron::ui`):
  - `update::CheckPolicy load_update_policy(const QSettings&);` and `void save_update_policy(QSettings&, const update::CheckPolicy&);` — keys `updates/check_on_startup` (bool, default true), `updates/last_check` (ISO 8601 UTC, `Qt::ISODate`; unparsable → none), `updates/skipped_version` (text; `parse_version` fails → none).
  - `class UpdateChecker : public QObject` with
    - `using ClientFactory = std::function<std::unique_ptr<http::IHttpClient>()>;`
    - `UpdateChecker(QSettings& settings, update::Version running, std::function<std::chrono::system_clock::time_point()> now, ClientFactory make_client, QObject* parent = nullptr);`
    - `void check_on_startup();` — returns at once unless `due()` and the factory gives a client; else starts the worker.
    - `void check_now();` — manual: ignores `due()`; if no client, emits `failed` with `this build has no network client`.
    - `bool busy() const;`
    - `void skip(update::Version);` — stores `skipped`.
    - signals: `void available(const pychron::update::Release&, bool manual);` (automatic: only when `worth_showing`; manual: whenever newer than running), `void up_to_date(bool no_release);` (manual only), `void failed(const QString& reason, bool manual);`
    - destructor joins the worker.
  - The worker is a `std::thread` owned by the checker; it posts its `Result<Fetched>` back with `QMetaObject::invokeMethod(this, functor, Qt::QueuedConnection)`; the GUI-thread handler sets `last_check` (on `Found`/`NoRelease` only), saves, and emits. One worker at a time: `check_now` while busy does nothing. Automatic failures go to the log at info (network, status) or warning (unparsable), through the logger the UI already uses.

- [ ] **Step 1: Write the failing tests** `tests/ui/test_update_checker.cpp`, suite `UpdateChecker`, with `SettingsGuard` (`tests/ui/settings_guard.hpp`), `FakeHttp`, a fixed `now`, and `QSignalSpy` waiting with a 5 s real-time bound:
  - `PolicyRoundTrip`: save then load three keys equal.
  - `GarbageSettingsReadAsNone`: `last_check = "yesterday"`, `skipped_version = "banana"` → both none; missing `check_on_startup` → true.
  - `StartupNotDueSendsNothing`: `last_check = now - 1h` → `fake.sent` empty, no signal.
  - `StartupDisabledSendsNothing`.
  - `StartupNewerEmitsAvailable`: tag `v999.0.0` → one `available` with `manual == false`; `updates/last_check` now equals `now`.
  - `StartupSkippedEmitsNothing`: skipped `999.0.0`, tag `v999.0.0` → no signal, `last_check` still written.
  - `StartupFailureIsSilentAndKeepsLastCheck`: `Io` → no `available`, `failed(manual=false)` only, `last_check` unchanged.
  - `ManualIgnoresDueAndSkipped`: `last_check = now`, skipped `999.0.0`, tag `v999.0.0` → `available(manual=true)`.
  - `ManualUpToDate` and `ManualNoRelease` → `up_to_date(false)` / `up_to_date(true)`.
  - `NoClient` (Review Focus 5): factory returns null → startup: no signal; manual: `failed` with `this build has no network client`.
  - `DestroyedWhileInFlight` (Review Focus 3): `fake.hold()`, tag `v999.0.0`, `check_now()`; connect `available` to a counter; start a `std::thread` that calls `fake.release()` (timing does not matter: the destructor must be correct whether the answer arrives before or during the join); `delete` the checker on the GUI thread; join the helper; `QCoreApplication::processEvents()`; expect the counter is 0 and nothing crashed (the ASan and TSan runs are what make this test bite).
- [ ] **Step 2: Run, see it fail** (`cmake --build --preset dev-ui --target pychron_ui_tests`).
- [ ] **Step 3: Implement** `update_checker.{hpp,cpp}` to the interface above.
- [ ] **Step 4: Run** `build/dev-ui/tests/ui/pychron_ui_tests --gtest_filter='UpdateChecker.*'`, expected PASS.
- [ ] **Step 5: Add the real-time entry** to section 4.4 of the virtual-clock spec: the update check's worker thread and its 10 s request timeout (`apps/pychron-ui/src/update_checker.cpp`): a real socket, not on a simulated path.
- [ ] **Step 6: Thread sanitizer**: configure a `build/tsan` tree with `-DPYCHRON_SANITIZE=thread` and the UI on, build `pychron_ui_tests`, run `--gtest_filter='UpdateChecker.*'`; expected PASS with no TSan report.
- [ ] **Step 7: Static analysis** (`tools/compiler_sweep.py --changed --build-dir build/dev-ui`).
- [ ] **Step 8: Commit** `feat(ui): check for a newer release on a worker thread`

### Task 7: pychron-ui indicator, dialog, menu and preference

**Files:**
- Create: `apps/pychron-ui/src/update_ui.hpp`, `apps/pychron-ui/src/update_ui.cpp`, `tests/ui/test_update_ui.cpp`
- Modify: `apps/pychron-ui/src/brand.cpp` (`add_help_menu`: a *Check for Updates…* action beside About, `MenuHub::Scope::App`), `apps/pychron-ui/src/preferences.hpp`/`.cpp` (field `bool check_for_updates = true;` read from and written to `updates/check_on_startup`, so the same key Task 6 reads), `apps/pychron-ui/src/preferences_dialog.cpp` (checkbox "Check for updates at startup", tooltip: `Once a day, asks api.github.com for the latest pychron release. Sends only the program's version.`), `apps/pychron-ui/src/main.cpp` (after `window.show()` in both the line window and the data window paths: create the checker and indicator for that window, call `check_on_startup()`), `apps/pychron-ui/CMakeLists.txt`, `tests/ui/test_preferences.cpp`, `docs/user/01-getting-started.md` (or `07-configuration-reference.md`: a short "Updates" section: what is checked, when, what is sent, how to turn it off, that nothing is downloaded)

**Interfaces:**
- Consumes: `UpdateChecker` (Task 6), `CoreBridge::wall_now()` for `now` on the line window; on the data window (no line, no bridge) the `wall_now()` of a `pychron::SteadyClock`. Never `QDateTime::currentDateTime()`.
- Produces:
  - `class UpdateIndicator : public QToolButton` — hidden until `show_release(const update::Release&)`; text `Update available: v<x.y.z>`; click opens `UpdateDialog`; `hide()` after skip.
  - `class UpdateDialog : public QDialog` — modes: release (title `pychron <x.y.z> is available`, notes as markdown in a read-only `QTextBrowser`, buttons *Download* → `QDesktopServices::openUrl(html_url)`, *Skip this version* → `checker.skip(version)` and indicator hidden, *Close*), up to date (`pychron <running> is up to date` / `No release has been published yet`), failure (`Could not check for updates: <reason>`; network errors read `Could not reach GitHub`).
  - `void install_update_check(QMainWindow* window, UpdateChecker& checker);` — adds the indicator with `statusBar()->addPermanentWidget`, connects automatic `available` to the indicator only (never a dialog), manual signals to `UpdateDialog`, and the Help action to `check_now()` (disabled while `busy()`).

- [ ] **Step 1: Write the failing tests** `tests/ui/test_update_ui.cpp`, suite `UpdateUi`, with a fake checker fed by `FakeHttp`:
  - `IndicatorHiddenAtStart`.
  - `AutomaticAvailableShowsIndicatorOnly`: emit `available(release, false)` → indicator visible with `Update available: v999.0.0`; `QApplication::topLevelWidgets()` contains no visible `UpdateDialog`.
  - `SkipHidesIndicator`: open the dialog from the indicator, press *Skip this version* → indicator hidden, `updates/skipped_version == "999.0.0"`.
  - `ManualUpToDateShowsDialog`: emit `up_to_date(false)` → `UpdateDialog` visible saying `is up to date`.
  - `ManualFailureShowsReason`.
  - `HelpActionPresent`: the Help menu of a `QMainWindow` after `add_help_menu` has an action `Check for Updates…`.
  - In `tests/ui/test_preferences.cpp`: `check_for_updates` round-trips through `updates/check_on_startup`; missing key → true.
- [ ] **Step 2: Run, see it fail.**
- [ ] **Step 3: Implement** `update_ui.{hpp,cpp}`, the menu action, the preference field and checkbox, and the `main.cpp` wiring. `QDesktopServices::openUrl` is the only thing the *Download* button does.
- [ ] **Step 4: Run** `build/dev-ui/tests/ui/pychron_ui_tests --gtest_filter='UpdateUi.*:Preferences*'`, expected PASS.
- [ ] **Step 5: Document** the Updates section in the user guide.
- [ ] **Step 6: Try it**: `build/dev-ui/apps/pychron-ui/pychron-ui --sim` with `updates/last_check` removed; the indicator should not appear for an up-to-date build, and *Help → Check for Updates…* shows the up-to-date dialog against the real API.
- [ ] **Step 7: Static analysis** (`--build-dir build/dev-ui`).
- [ ] **Step 8: Commit** `feat(ui): update indicator, dialog and Check for Updates`

### Task 8: Whole-branch verification

**Files:** none new.

- [ ] **Step 1:** Rebase on `origin/develop`.
- [ ] **Step 2:** `python3 tools/quality_check.py` → exit 0.
- [ ] **Step 3:** `python3 tools/compiler_sweep.py --build-dir build/dev` and `--build-dir build/dev-ui` → no warnings.
- [ ] **Step 4:** `cmake --build --preset dev-ui && ctest --preset dev-ui` → all pass (covers `dev` too).
- [ ] **Step 5:** A build with `-DPYCHRON_SANITIZE=address,undefined` runs `pychron_update_tests`, `pychron_elctl_tests --gtest_filter='VersionCmd.*'`, `pychron_ui_tests --gtest_filter='Update*'` clean.
- [ ] **Step 6:** Check the spec still matches the code; amend it in this branch where it does not.

## Open questions

None.
