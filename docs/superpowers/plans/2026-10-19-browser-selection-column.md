# Data browser Filtered and Selected columns: implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split the data browser's analysis table into a Filtered list (the query result) and a Selected list the user builds across many queries; figure, export and publish act on Selected.

**Architecture:** A uuid filter is added to the browse path (persistence `BrowseFilter`, processing `BrowseQuery`, both sources) so Selected can be refetched by id when the store changes. A small Qt-free `SelectionList` keeps Selected's rows (sorted, unique). `DataBrowserWindow` gets a second `AnalysisTableModel` and table, an Add/Remove button strip, and its actions read Selected.

**Tech Stack:** C++20, Qt 6 Widgets, QtTest (UI), GoogleTest (persistence, processing), TinyORM on QtSql (SQLite, PostgreSQL).

**Spec:** `docs/superpowers/specs/2026-10-19-browser-selection-column-design.md`

## Global Constraints

- Rules of root `AGENTS.md`, `libs/persistence/AGENTS.md` ("Schema and queries") and `apps/pychron-ui/AGENTS.md` apply.
- `uuids` empty → browse SQL text and bindings identical to today's (spec rule 5).
- Store uuid lookup in parts of 500 values (`kPart`, as `refs.cpp` `derived_fingerprints`).
- Selected never holds one uuid twice; ordered newest first, `timestamp` desc then `uuid` desc.
- Figure, export and publish send exactly Selected's uuids, sorted (as `selected_uuids()` sorts today); never anything from Filtered.
- `Add all` confirm threshold: more than 1000 matching (`total_` known).
- Selected not persisted: no `QSettings` key.
- Static analysis (`python3 tools/quality_check.py`) and `tools/compiler_sweep.py --changed --build-dir build/dev-ui` after each C++ task.
- UI tasks build with preset `dev-ui`; final run `ctest --preset dev-ui`.

## Review Focus

1. Selected row whose analysis was deleted while browser was hidden: on next show the refresh drops it and says so (Task 6 test `browser_selected_drops_a_removed_analysis_when_shown`).
2. Filter change while rows highlighted in Selected: Selected's highlight and contents untouched (Task 4 test `browser_selected_survives_a_filter_change` asserts highlight too).
3. Removing rows while a time-break separator sits between them: separators not counted as analyses, removed rows exactly the highlighted analyses (Task 4 test `browser_remove_skips_separators`).
4. `Add all` when the browse fails half way: rows fetched before the error added, status shows error (Task 7 test `browser_add_all_keeps_what_came_before_an_error`).
5. Store with more than 500 selected analyses refetched: every one comes back, in order (Task 1 test `UuidFilterInParts`, Task 6 test uses 600 through `MemorySource`).

---

### Task 1: Uuid filter in the store's browse

**Files:**
- Modify: `libs/persistence/include/pychron/persistence/store.hpp:512-522` (`BrowseFilter`)
- Modify: `libs/persistence/src/browse.cpp` (`build_where`, `browse`)
- Modify: `libs/persistence/src/sql/browse.hpp` (clause constant)
- Test: `tests/persistence/test_browse.cpp`

**Interfaces:**
- Produces: `std::vector<Uuid> BrowseFilter::uuids;` — non-empty: only those analyses, ANDed with every other field. `browse()` answers any count of uuids.

- [ ] **Step 1: Write the failing tests** (`TEST_P(BrowseTest, ...)`, fixture analyses `a1_`..`a5_`, `a4_` tagged invalid):

```cpp
TEST_P(BrowseTest, UuidFilter) {
  BrowseFilter f;
  f.uuids = {a1_, a4_, a2_};
  EXPECT_EQ(uuids(browse(f)), (std::vector<Uuid>{a4_, a2_, a1_}));  // page order, invalid kept
  EXPECT_EQ(browse(f).total, 3);
  f.exclude_tags = {"invalid"};
  EXPECT_EQ(uuids(browse(f)), (std::vector<Uuid>{a2_, a1_}));       // ANDed with other fields
  f = {};
  f.uuids = {Uuid::v7()};
  EXPECT_TRUE(browse(f).rows.empty());
}

TEST_P(BrowseTest, UuidFilterInParts) {
  BrowseFilter f;
  for (int i = 0; i < 1200; ++i) f.uuids.push_back(Uuid::v7());
  f.uuids.push_back(a3_);
  f.uuids.insert(f.uuids.begin() + 700, a5_);
  f.uuids.push_back(a1_);
  const auto r = browse(f, 1203);
  EXPECT_EQ(uuids(r), (std::vector<Uuid>{a5_, a3_, a1_}));
  EXPECT_FALSE(r.next);
}

TEST_P(BrowseTest, NoUuidsLeavesTheStatementAlone) {
  // browse_page_statement / browse_count_statement / facet_statement with
  // f.uuids empty: sql and bindings equal those of a filter that never had the field.
}
```


- [ ] **Step 2: Run, expect FAIL (no member `uuids`)**

Run: `cmake --build --preset dev --target pychron_persistence_tests && build/dev/tests/persistence/pychron_persistence_tests --gtest_filter='*BrowseTest.Uuid*:*BrowseTest.NoUuids*'`

- [ ] **Step 3: Implement**

- `BrowseFilter`: `std::vector<Uuid> uuids;  // non-empty: only these analyses` after `age_max`.
- `sql/browse.hpp`: `kBrowseUuidIn = QStringLiteral("a.uuid IN (%1)")`.
- `build_where`: when `f.uuids` non-empty, clause with `sql::placeholders(n)`, one `qv(uuid)` binding each. Last clause, so empty case changes nothing.
- `browse()`: when `req.filter.uuids.size() > kPart` (500): for each part, a copy of `req` with that part, `after` empty, `limit = part size`, `count_total = false`; run page statement; collect rows; sort by `timestamp` desc then `uuid` desc; no `total`, no `next` (spec 4.6: `count_total` ignored). Otherwise unchanged path.

- [ ] **Step 4: Run, expect PASS** (same command). Then whole persistence binary; with `PYCHRON_TEST_PG_URL` set as in memory note `postgres-for-tests` for the PostgreSQL parameter.

- [ ] **Step 5: Commit** `feat(persistence): browse by a list of analysis uuids`

---

### Task 2: Uuid filter in both sources, and MemorySource::remove

**Files:**
- Modify: `libs/processing/include/pychron/processing/source.hpp` (`BrowseQuery`, `MemorySource`)
- Modify: `libs/processing/src/source.cpp` (`matches`, `MemorySource::remove`)
- Modify: `libs/processing/adapters/store/src/store_source.cpp` (`to_store_filter`, `browse`, `facet`)
- Test: `tests/processing/test_units.cpp`, `tests/processing/test_store_source.cpp`

**Interfaces:**
- Consumes: `BrowseFilter::uuids` (Task 1).
- Produces: `std::vector<std::string> BrowseQuery::uuids;` (same meaning). `void MemorySource::remove(const std::string& uuid);` — drops analysis and its raw data and age, bumps `generation_`; unknown uuid: nothing, no bump.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(MemorySource, BrowsesByUuid) {
  // five analyses; q.uuids = {u1, u4, u2} → rows u4, u2, u1 (newest first), total 3;
  // with q.exclude_tags = {} an invalid one is returned; q.uuids = {"nope"} → empty.
}
TEST(MemorySource, RemoveDropsTheAnalysisAndMovesTheGeneration) {
  // remove(u2): browse lacks u2, generation() + 1; remove("nope"): generation unchanged.
}
TEST_F(StoreSourceTest, BrowsesByUuid) {
  // fixture's analyses; q.uuids two of them, q.exclude_tags = {} → those two, newest first.
  // q.uuids = {"not-a-uuid"} → browse fails with ErrorKind::Config.
}
```

- [ ] **Step 2: Run, expect FAIL**

Run: `cmake --build --preset dev --target pychron_processing_tests && build/dev/tests/processing/pychron_processing_tests --gtest_filter='MemorySource.*:StoreSourceTest.BrowsesByUuid'`

- [ ] **Step 3: Implement**

- `BrowseQuery`: `std::vector<std::string> uuids;  // non-empty: only these analyses` after `age_max`.
- `matches`: `!q.uuids.empty()` and `s.uuid` not in it → false. `browse_summaries` and `RecordSource` follow from it.
- `StoreSource`: helper `Result<std::vector<ps::Uuid>> store_uuids(const BrowseQuery&)` using `parse_uuid`; failure → `fail(ErrorKind::Config, "browse: not a uuid: <text>")`. `browse` and `facet` set `filter.uuids` from it.
- `MemorySource::remove` under `mutex_`.

- [ ] **Step 4: Run, expect PASS** (same command), then whole processing binary.

- [ ] **Step 5: Commit** `feat(processing): browse by a list of analysis uuids`

---

### Task 3: SelectionList

**Files:**
- Create: `apps/pychron-ui/src/selection_list.hpp`, `apps/pychron-ui/src/selection_list.cpp`
- Modify: `apps/pychron-ui/CMakeLists.txt` (both lists, beside `data_browser_window`)
- Test: `tests/ui/test_selection_list.cpp` (globbed in, target `pychron_ui_test_selection_list`)

**Interfaces:**
- Produces, namespace `pychron::ui`, no Qt:

```cpp
class SelectionList {
 public:
  // Adds rows not already held; returns how many were added.
  std::size_t add(std::vector<processing::AnalysisSummary> rows);
  // Returns how many were removed.
  std::size_t remove(const std::vector<std::string>& uuids);
  void clear() noexcept;
  // Rows refetched from the source replace the held ones; held uuids missing
  // from `fresh` are dropped. Returns how many were dropped.
  std::size_t replace_from(std::vector<processing::AnalysisSummary> fresh);
  bool contains(const std::string& uuid) const;
  const std::vector<processing::AnalysisSummary>& rows() const noexcept;  // newest first
  std::vector<std::string> uuids() const;   // sorted ascending
  std::size_t size() const noexcept;
  bool empty() const noexcept;
};
```

- [ ] **Step 1: Write the failing test** (QtTest class `SelectionListTest`):
  - `add_sorts_newest_first_and_ignores_duplicates`: add {t=1 "a", t=3 "c"}, then {t=2 "b", t=3 "c"} → second add returns 1; `rows()` uuids c, b, a.
  - `equal_timestamps_order_by_uuid_descending`: two rows t=5 "x", "y" → y, x.
  - `remove_and_clear`: remove {"b", "nope"} returns 1; clear → empty.
  - `replace_from_updates_and_drops`: hold a, b, c (b tag "ok"); `replace_from({b tag "invalid", c})` returns 1; rows c, b; b's tag "invalid".
  - `uuids_are_sorted`: hold c, a, b → `uuids()` a, b, c.

- [ ] **Step 2: Run, expect FAIL** — `cmake --build --preset dev-ui --target pychron_ui_test_selection_list` fails (no header).

- [ ] **Step 3: Implement** `selection_list.cpp`: rows in a vector kept sorted by `(timestamp desc, uuid desc)`, plus `std::unordered_set<std::string>` of uuids.

- [ ] **Step 4: Run, expect PASS** — `build/dev-ui/tests/ui/pychron_ui_test_selection_list`

- [ ] **Step 5: Commit** `feat(ui): a list of analyses kept across browser queries`

---

### Task 4: Two lists, Add / Remove / Clear, mark

**Files:**
- Modify: `apps/pychron-ui/src/analysis_table_model.hpp/.cpp` (mark)
- Modify: `apps/pychron-ui/src/data_browser_window.hpp/.cpp`
- Test: `tests/ui/test_selection_list.cpp` (model mark), `tests/ui/test_data_windows.cpp`

**Interfaces:**
- Consumes: `SelectionList` (Task 3).
- Produces on `AnalysisTableModel`: `void set_marked(std::unordered_set<std::string> uuids);` — `data(index(r, RunId), Qt::DecorationRole)` is `QApplication::style()->standardIcon(QStyle::SP_DialogApplyButton)` for an analysis row whose uuid is marked, invalid `QVariant` otherwise and for separators; emits `dataChanged` on column `RunId`, `Qt::DecorationRole`.
- Produces on `DataBrowserWindow`:
  - `QStringList highlighted_uuids() const;` — today's `selected_uuids()` renamed: highlighted rows of Filtered, sorted.
  - `select_rows(const QList<int>&)` unchanged (highlights Filtered).
  - `void add_highlighted();` `void remove_highlighted();` `void clear_selected();`
  - `QStringList selection_uuids() const;` — `SelectionList::uuids()` as `QStringList`.
  - `void select_selected_rows(const QList<int>& rows);` — highlights Selected by index into its rows (tests).
  - Accessors for tests: `AnalysisTableModel* selected_model() const`, `QTableView* selected_table() const`, `QPushButton* add_button() / add_all_button() / remove_button() / clear_button() const`, `QLabel* selected_count() const`.
  - `model()` and `table()` stay Filtered's.

- [ ] **Step 1: Write the failing tests**

In `test_selection_list.cpp`: `model_marks_the_run_id_of_marked_rows` — three rows, `set_marked({"b"})` → only b's RunId cell has a valid `DecorationRole`; other columns none.

In `test_data_windows.cpp` (`make_source(n)` gives `uuid-0`..`uuid-(n-1)`, newest = highest):
  - `browser_add_and_remove`: `make_source(10)`; `select_rows({0, 2})`; click `add_button()` → `selection_uuids()` == {uuid-7, uuid-9}; `selected_count()->text()` contains "2 selected"; again `select_rows({0})`, add → still 2, `status()` text contains "already selected"; `select_selected_rows({0})`, click `remove_button()` → {uuid-7}; click `clear_button()` → empty, label "0 selected".
  - `browser_selected_survives_a_filter_change`: add uuid-9; `select_selected_rows({0})`; `search()->setText("no such run")` → `model()->analysis_count()` 0, `selection_uuids()` {uuid-9}, `selected_table()->selectionModel()->selectedRows().size()` 1.
  - `browser_gathers_from_two_queries`: two queries by search text (`make_source` run ids; pick two prefixes that split the rows), add from each → Selected holds both, `selected_model()->rows()` newest first.
  - `browser_marks_selected_rows_in_filtered`: add row 0 → Filtered row 0 RunId cell has DecorationRole, row 1 not; remove → none; mark survives `load_more` and a search change that keeps the row.
  - `browser_enter_adds_and_delete_removes`: focus `table()`, current row 0, `QTest::keyClick(table(), Qt::Key_Return)` → 1 selected, no `recall_requested`; focus `selected_table()`, highlight row 0, `Qt::Key_Delete` → 0; repeat with `Qt::Key_Backspace`.
  - `browser_remove_skips_separators`: source with a time break (as `browser_time_breaks` tests build one, `set_gap_hours`), add all rows, highlight in Selected rows on both sides of a separator via `select_selected_rows`, remove → exactly those analyses gone.

- [ ] **Step 2: Run, expect FAIL** — `cmake --build --preset dev-ui --target pychron_ui_test_data_windows pychron_ui_test_selection_list` (compile errors).

- [ ] **Step 3: Implement**

- Layout: right pane of today's splitter becomes a horizontal `QSplitter` with three widgets: Filtered (banner, `table_`, status, Load more, as now), button strip (`QVBoxLayout`: `Add →`, `Add all`, `← Remove`, `Clear`, stretch), Selected (`selected_count_` label over `selected_table_`). `selected_table_` set up as `table_` is (selection behaviour/mode, span of breaks, colouring, gap threshold, type colours: every setter that touches `model_` touches `selected_model_` too).
- `add_highlighted`: summaries of highlighted Filtered rows (skip separators via `analysis_at`) into `selection_`; then `sync_selected()`; status `Added %n` plus ` (%1 already selected)` when some were held.
- `sync_selected()` (private): `selected_model_->set_rows(selection_.rows())`, `model_->set_marked(...)`, count label `tr("%n selected")`, action enable state (Task 5 uses it).
- Mark after `reload`, `load_more`, `refresh_changed`: `set_rows` keeps the marked set (store it in the model, not per row).
- Keys: `QShortcut` with context `Qt::WidgetShortcut` on `table_` for `Key_Return` and `Key_Enter` → `add_highlighted`; on `selected_table_` for `Key_Delete`, `Key_Backspace` → `remove_highlighted`.
- Recall: replace `connect(table_, &QTableView::activated, ...)` (line 282) with `doubleClicked` on both tables; Space shortcut (`Qt::WidgetShortcut`) on both. `recall_current` and `recall_step` act on the table with focus (`selected_table_->hasFocus()` → Selected, else Filtered).
- Rename `selected_uuids` → `highlighted_uuids` and update callers (`place()`, refresh test at `test_data_windows.cpp:805`).

- [ ] **Step 4: Run, expect PASS** — `build/dev-ui/tests/ui/pychron_ui_test_data_windows browser_add_and_remove browser_selected_survives_a_filter_change browser_gathers_from_two_queries browser_marks_selected_rows_in_filtered browser_enter_adds_and_delete_removes browser_remove_skips_separators` and the selection_list binary.

- [ ] **Step 5: Commit** `feat(ui): the data browser's Selected list`

---

### Task 5: Actions read Selected

**Files:**
- Modify: `apps/pychron-ui/src/data_browser_window.hpp/.cpp` (`request_figure`, `request_export`, publish, enable state; delete `export_uuids`)
- Test: `tests/ui/test_data_windows.cpp`, `tests/ui/test_earthbank_publish.cpp`

**Interfaces:**
- Consumes: `selection_uuids()`, `add_highlighted()`, `sync_selected()` (Task 4).
- Produces: `figure_requested`, `export_requested`, `publish_requested` carry `selection_uuids()`. Every `plot_action(kind)`, `export_action()`, `publish_action()` enabled exactly while Selected non-empty. `export_uuids()` removed.

- [ ] **Step 1: Write and change the tests**
  - New `browser_actions_take_the_selected_list`: `make_source(10)`; all figure actions, export, publish disabled; highlight rows 0, 2 in Filtered without adding → still disabled; add → enabled; spectrum trigger sends {uuid-7, uuid-9}; change search so Filtered shows other rows, highlight one there → spectrum still sends {uuid-7, uuid-9}; clear → disabled.
  - Replace `export_uuids_are_the_selection_else_the_shown_rows` (line 2220) with the above's export part; `browser_export_button_asks_for_a_file_and_emits` adds before triggering; its "nothing selected: everything shown" part becomes "nothing selected: action disabled".
  - `browser_toolbar_offers_every_figure_recall_and_export`: before the loop, add all four rows (`select_rows({0,1,2,3})`, `add_highlighted()`); comment "nothing selected: all that is shown" goes.
  - `browser_recall_and_time_series_signals`: add rows 0, 2 before spectrum trigger.
  - Every other use of `select_rows` followed by an action trigger in `test_data_windows.cpp` (lines 911, 1002, 1116, 1184 at time of writing) and `test_earthbank_publish.cpp` (111-118, 717, 740, 752): call `add_highlighted()` after `select_rows`, compare against `selection_uuids()`. In `test_earthbank_publish.cpp:118` "`select_rows({})` → disabled" becomes `clear_selected()` → disabled. At line 752 (second publish), `clear_selected()` before `select_rows({0, 1})` and add.

- [ ] **Step 2: Run, expect FAIL** — new test fails (actions enabled, Filtered sent).

- [ ] **Step 3: Implement** — `request_figure`, `request_export`, publish lambda read `selection_uuids()`; return without emitting when empty. Enable state set in `sync_selected()` and in the constructor (start disabled); drop today's enable connection on Filtered's `selectionChanged`/`modelReset` (line 240). Tooltips: figure, Export, Publish name "the selected analyses (right list)". Remove `export_uuids` and its header comment.

- [ ] **Step 4: Run, expect PASS** — whole `pychron_ui_test_data_windows` and `pychron_ui_test_earthbank_publish` binaries.

- [ ] **Step 5: Commit** `feat(ui)!: figures, export and publish take the browser's Selected list`

  Body: `BREAKING CHANGE: Enter in the browser adds to Selected instead of recalling; actions no longer fall back to the shown rows.`

---

### Task 6: Selected follows the store

**Files:**
- Modify: `apps/pychron-ui/src/data_browser_window.cpp` (`refresh_changed`, `refresh`)
- Test: `tests/ui/test_data_windows.cpp`

**Interfaces:**
- Consumes: `BrowseQuery::uuids`, `MemorySource::remove` (Task 2); `SelectionList::replace_from` (Task 3); `sync_selected()` (Task 4).
- Produces: private `void refetch_selected();`

- [ ] **Step 1: Write the failing tests**
  - `browser_selected_shows_a_new_tag`: `make_source(10)`; add uuid-9; replace it in the source (`src->add` of a copy with `tag = "invalid"`); `w.refresh()` → Selected still holds uuid-9, its row's `tag` "invalid"; highlighted Selected row still highlighted.
  - `browser_selected_drops_a_removed_analysis`: add uuid-9, uuid-8; `src->remove("uuid-8")`; `w.refresh()` → `selection_uuids()` {uuid-9}; status contains "1 selected analyses no longer in store" (use `tr("%n selected analyses no longer in store")`).
  - `browser_selected_drops_a_removed_analysis_when_shown`: as above, but window hidden, `refresh_if_changed()`, then `show()` → dropped on show.
  - `browser_refetches_many_selected`: `make_source(600)`, Load more until all shown, add all 600 highlighted; `src->add` a newer one; `refresh()` → still 600, order newest first.
  - `browser_selected_kept_when_refetch_fails`: source failing browse calls with non-empty `uuids`; add uuid-9, bump generation, `refresh()` → Selected unchanged, status starts "Error:".
  - `browser_filter_change_does_not_refetch_selected`: `CountingSource` (exists in this file) counts browse calls with non-empty `uuids`; search change → 0 such calls.

- [ ] **Step 2: Run, expect FAIL**

- [ ] **Step 3: Implement** `refetch_selected()`: nothing when `selection_` empty; `BrowseQuery q` with only `uuids = selection_.uuids()`, `exclude_tags = {}`, `limit = size`; on error status `Error: …`, Selected unchanged; else `replace_from`, keep Selected's highlight and scroll by uuid (as `select_again`), `sync_selected()`, status on dropped. Called from `refresh_changed` after Filtered when generation moved, and from `refresh()`. Never from `reload()`.

- [ ] **Step 4: Run, expect PASS** — the six test functions, then whole binary.

- [ ] **Step 5: Commit** `feat(ui): the browser's Selected list follows the store`

---

### Task 7: Add all

**Files:**
- Modify: `apps/pychron-ui/src/data_browser_window.hpp/.cpp`
- Test: `tests/ui/test_data_windows.cpp`

**Interfaces:**
- Consumes: `query()`, `page_size_`, `total_`, `sync_selected()`.
- Produces: `void add_all();` and test hook `std::function<bool(std::size_t count)> confirm_add_all;` (default: `QMessageBox::question`, text `tr("Add all %n analyses to Selected?")`; true adds).

- [ ] **Step 1: Write the failing tests**
  - `browser_add_all_walks_every_page`: `make_source(260)` (page 200) → click `add_all_button()` → 260 selected; Filtered still 200 rows loaded.
  - `browser_add_all_asks_above_1000`: `make_source(1001)`; `confirm_add_all` returns false and records count → called with 1001, nothing added; returns true → 1001 added. `make_source(1000)` → hook not called.
  - `browser_add_all_keeps_what_came_before_an_error`: a source (subclass `MemorySource` as `CountingSource` does) failing the second `browse` with an `after` cursor; 260 rows → 200 added, status starts "Error:".

- [ ] **Step 2: Run, expect FAIL**

- [ ] **Step 3: Implement** — when `total_` and `*total_ > 1000`, ask `confirm_add_all(*total_)`; false → return. Walk `source_.browse(q)` from `query()` with `limit = page_size_`, `after` = previous `next`, until `next` empty or error; add collected rows; `sync_selected()`; status as `add_highlighted`, or error text.

- [ ] **Step 4: Run, expect PASS**, then whole binary.

- [ ] **Step 5: Commit** `feat(ui): add every analysis a query matches to Selected`

---

### Task 8: User guide and final run

**Files:**
- Modify: `docs/user/06-data-analysis.md` (browser section)

- [ ] **Step 1:** Describe Filtered and Selected, Add / Add all / Remove / Clear, Enter adds, Delete removes, double-click or Space recalls, figure/export/publish use Selected, Selected kept until quit, a run tagged invalid stays.
- [ ] **Step 2:** `python3 tools/quality_check.py` → exit 0; `python3 tools/compiler_sweep.py --build-dir build/dev-ui` → no warnings.
- [ ] **Step 3:** `cmake --build --preset dev-ui && ctest --preset dev-ui` → all pass. Persistence suite also with `PYCHRON_TEST_PG_URL` set.
- [ ] **Step 4: Commit** `docs: the data browser's Selected list in the user guide`

## Open questions

None.
