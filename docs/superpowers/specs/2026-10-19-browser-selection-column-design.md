# Data browser: Filtered and Selected columns: design

Date: 2026-10-19. Status: design agreed with developer 2026-10-19.

Builds on `2026-10-02-data-browsing-visualization-design.md` §11.2 (browser)
and `2026-10-09-data-browser-search-display-design.md`. Step toward §9.3
(saved selections), which stays unbuilt.

## 1. Problem

- Browser (`apps/pychron-ui/src/data_browser_window.cpp`) has one table.
  Selection is its `QItemSelectionModel`; `reload()` (every filter change)
  calls `model_->set_rows` and selection is gone. Only `refresh_changed`
  restores it (`select_again`).
- So one set of analyses = one query. Unknowns of two levels, or unknowns
  plus their air shots, cannot be gathered for one figure, export or
  publish.
- Export falls back to "all shown" when nothing selected
  (`export_uuids`, line 607).

## 2. Goal

Right side of browser split in two: **Filtered** (query result, as today)
and **Selected** (what user put there). Selected survives any filter
change, so one set spans many filtering episodes. Figure, export, publish
act on Selected.

Developer's choices (2026-10-19):

- Actions read Selected only; disabled while it is empty.
- Moved by Add / Remove buttons and keys; no drag and drop.
- Selected lives until application quits; not written anywhere.
- Selected ordered as Filtered (newest first).
- Approach A: rows copied on Add, refetched by uuid when store changes.
- Analysis tagged invalid after it was added stays in Selected, shown by
  its tag.
- Enter in Filtered becomes Add (today: recall).

## 3. Layout

- Right pane of existing splitter becomes horizontal `QSplitter`:
  Filtered pane | button strip | Selected pane.
- Filtered pane: today's banner, table, status label, Load more. Unchanged
  but for mark (4.3).
- Button strip, top to bottom: `Add →`, `Add all`, `← Remove`, `Clear`.
- Selected pane: count label (`%n selected`), table.
- Both tables: same columns, `AnalysisTableModel`, colour-by, time breaks,
  `ExtendedSelection`, `SelectRows`.
- Toolbar unchanged. Figure, export, publish tooltips say "selected
  analyses (right list)".

## 4. Behaviour

### 4.1 Adding

- `Add →`: highlighted Filtered rows into Selected. Enter in Filtered does
  same.
- `Add all`: every analysis matching current query, all pages, not only
  loaded ones: walks `IAnalysisSource::browse` with `after` cursor, page
  size `page_size_`, until `next` empty. When `total_` is known and above
  1000, a confirm dialog naming the count comes first; on No, nothing
  added. A browse error stops the walk; rows fetched before it are added
  and status line gives error.
- Add copies `AnalysisSummary` from Filtered model: no query.
- Uuid already in Selected: ignored. Status line: `Added N (M already
  selected)` when M > 0.
- Selected kept sorted newest first (`timestamp` desc, `uuid` desc as
  tie-break, same order as browse page).

### 4.2 Removing

- `← Remove`: highlighted Selected rows out. Delete and Backspace in
  Selected do same.
- `Clear`: all out, no confirm.

### 4.3 Mark

Filtered row whose uuid is in Selected shows check mark in first column
(`Qt::DecorationRole`). Updated on add, remove, clear, reload, load more.

### 4.4 Actions

- `figure_requested`, `export_requested`, `publish_requested` carry
  Selected's uuids (sorted, as `selected_uuids()` today).
- Figure, export, publish actions disabled while Selected empty; enabled
  state follows Selected model's `rowsInserted`, `rowsRemoved`,
  `modelReset`.
- Export's "else all shown" fallback removed (`Add all` then Export is the
  same).
- Recall (double-click, Space) and Ctrl+N / Ctrl+B act on table that has
  focus; current row of that table.
- Recall wired to `QTableView::doubleClicked` and a Space shortcut, not to
  `activated` as today (line 282): `activated` also fires on Enter, which
  now adds.

### 4.5 Refresh

- `refresh_changed`, when generation moved: Filtered as today, then
  Selected refetched by uuid (4.6) with no other filter and
  `exclude_tags = {}`.
- Refetched rows replace copies (new tag, cached age, etc). Selected's
  highlighted rows and scroll kept (by uuid, as `select_again`).
- Uuid no longer in store: dropped; status line `N selected analyses no
  longer in store`.
- Refetch error: Selected unchanged, status line gives error.
- `reload()` (filter change) never touches Selected.

### 4.6 Uuid filter

- `processing::BrowseQuery` and `persistence::BrowseFilter` gain
  `std::vector<std::string> uuids`. Non-empty: only analyses with those
  uuids match; ANDed with every other field. Empty: no effect (today's
  behaviour).
- Store: `a.uuid IN (?, ?, ...)`, each uuid bound. A request with more
  uuids than one statement may bind is run in chunks (size as
  `refs.cpp`); `browse` merges chunks into one page in page order, no
  cursor, `count_total` ignored. Lookup by primary key: no schema change,
  no index.
- `StoreSource::browse` passes `uuids` through. `MemorySource::browse`
  filters same.
- Selected refetch sets `limit` to uuid count.

### 4.7 State

Selected model and its uuid set owned by `DataBrowserWindow`.
`DataWorkspace` builds browser once and keeps it until destroyed, so
Selected lives until quit. No `QSettings` key.

## 5. Rules that must hold

1. No filter change, reload or Load more changes Selected.
2. Selected never holds one uuid twice.
3. Figure, export and publish send exactly Selected's uuids. They are never
   sent anything from Filtered.
4. After a refresh, a Selected row shows the store's current summary or is
   gone, never a copy older than the generation shown.
5. `uuids` empty → browse SQL text and bindings identical to today's.

## 6. Tests

`tests/ui/test_data_windows.cpp`:

- Add, then change filter: Selected unchanged; Filtered shows new query.
- Add from two queries: Selected holds both, newest first.
- Add same row twice: one row, status says already selected.
- `Add all` with more rows than one page: all matching rows added.
- `Add all` above 1000: confirm asked; No adds nothing.
- Remove, Delete key, Clear.
- Mark shows on Filtered rows in Selected, goes on remove.
- Figure, export, publish send Selected's uuids; disabled when empty.
- Enter in Filtered adds; double-click recalls; Ctrl+N steps focused table.
- Refresh after tag set invalid: row stays, tag shown. Refresh after
  analysis removed from source: row dropped, status says so.

`tests/persistence/test_browse.cpp`: uuid filter alone; with other
filters; more uuids than one chunk; empty `uuids` gives today's
statement. Run on SQLite and on PostgreSQL where `PYCHRON_TEST_PG_URL` set.

Source tests (`MemorySource`, `StoreSource`): `uuids` passed through and
applied.

## 7. Out of scope

- Named or saved selections, last N kept (old spec §9.3).
- Selected kept across restart.
- Drag and drop between tables.
- Live link from Selected to an open figure window.
- Order of Selected other than newest first; grouping.

## Open questions

None.
