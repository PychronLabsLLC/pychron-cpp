# Conditionals editor and assignment

Date: 2026-10-03
Status: Implemented
Owner: Jake Ross
Depends on: `2026-10-02-conditionals-design.md` (model, files, levels,
validation; its section 9 left UI editors out of scope),
`2026-10-02-experiment-window-design.md` (QueueTableModel, RunFactoryPanel,
script editor pattern).

## 1. Goal

Edit a lab's conditionals files from `pychron-ui` without knowing the TOML
schema, and attach them to a queue and to runs from the experiment window.

Success: with `--lab configs/examples`, Experiment > Conditionals Editor opens
`system.toml`; adding a truncation `Ar40 > 8e5` with `start = 20` and saving
writes a file that `elctl conditionals-check` accepts; choosing that file
as the queue's conditionals, or ticking it for a run, shows up in the queue
file and the next run uses it.

## 2. Scope

In:

- A structured editor (table plus detail form) for every file in
  `<lab>/conditionals/`: system, queue and run level files are the same
  format and are edited the same way.
- Create, delete, save (no rename). Live check parsing and static validation
  against the lab's metric catalog.
- Assignment: the queue's conditionals file, and the conditionals of new and
  existing runs.

Out:

- Plan inline truncations and whiff blocks (they live in the plan file).
- A merged "what applies to this run" view.
- A text/source view of the TOML.
- Editing `ConditionalRef::kind`; pickers write names and keep the kind an
  existing reference already has.
- Renaming a file (references in queues would dangle).

## 3. Decisions

| # | Decision |
|---|---|
| D1 | Logic that does not need Qt lives in `libs/experiment`: the TOML writer and the file operations. The UI holds a `ConditionalSet` and calls them. |
| D2 | Saving writes canonical TOML. Comments and layout of a hand-written file are lost; the editor warns once per file before the first save that would drop comments. |
| D3 | The authored `check`, `window` and `mapper` are written, never the transformed expression. |
| D4 | A file is not saved while any conditional in it fails to compile, breaks a per-kind rule, or has an action whose parameters cannot be written (no name, no steps, a non-finite value); what would be written is parsed back before it replaces the file. Validation warnings and catalog errors (unknown isotope, ...) are shown but do not block: a file may be written for a lab state that does not exist yet. |
| D5 | Files may be edited while a queue runs. `ConditionalLibrary` reads the run-level and plan-level files per run, so a saved edit applies from the next run; the queue-wide sets (system, queue file) are loaded when the queue starts and apply from the next start. The status bar says which. Nothing is swapped into a run in progress. |
| D6 | Run assignment follows the queue table's existing edit rules (`row_editable`): frozen rows cannot change. The queue's conditionals file cannot change while the queue is live. |

## 4. Core additions (`libs/experiment`)

### 4.1 Writer

`conditionals/conditional.hpp`:

```cpp
// Canonical TOML for one file. parse_conditionals(to_toml(s)) yields the same
// items and disable list (level and location aside).
std::string to_toml(const ConditionalSet& set);
```

Layout: `disable = [...]` first when not empty, then one `[[table]]` block per
conditional, grouped by kind in the order truncations, terminations,
cancelations, actions, modifications, equilibrations, pre_run, post_run, and
in set order within a kind. Keys in a fixed order: `name`, `check`, `start`,
`frequency`, `ntrips`, `window`, `mapper`, `analysis_types`,
`abbreviated_count_ratio`, `action`, `resume`, `truncate`, `terminate`.

Omitted when default: `start = 0`, `frequency = 1`, `ntrips = 1`, no window,
empty mapper, empty analysis types, ratio 1.0, false flags, a `name` equal to
the default name the parser would give, and an `action` equal to the kind's
default (and always for terminations, cancelations and equilibrations, where
the parser rejects the key). Strings are escaped as TOML basic strings;
doubles are written with the shortest representation that round-trips.

### 4.2 Per-kind rules as data

The form needs to know which fields a kind takes without duplicating the
parser's rules. `conditional.hpp`:

```cpp
struct KindFields {
  bool gating;       // start, frequency: in-run kinds only
  bool ratio;        // abbreviated_count_ratio: truncation, equilibration, modification
  bool resume;       // action
  bool run_flags;    // truncate / terminate: modification
  std::vector<ActionSpec::Type> actions;  // allowed; empty = no action key
  ActionSpec::Type default_action;
};
const KindFields& fields_of(ConditionalKind k);

// Compiles the check and applies the per-kind rules the parser applies to a
// table; the error is the parser's message.
Result<Conditional> finalize(Conditional c);
```

The parser's per-kind switch is rewritten on top of `fields_of` and
`finalize` so there is one source of the rules. `ntrips`, `window`, `mapper`
and `analysis_types` apply to every kind.

### 4.3 Files

`conditionals/library.hpp`:

```cpp
// The files of <lab>/conditionals. Names are plain (no separators, no "..",
// no extension).
class ConditionalFiles {
 public:
  explicit ConditionalFiles(std::filesystem::path dir);
  Result<std::vector<std::string>> list() const;        // sorted names; empty when the directory is missing
  Result<std::string> read(std::string_view name) const;
  Result<void> write(std::string_view name, std::string_view text) const;  // temp file + rename; creates the directory
  Result<void> remove(std::string_view name) const;
  bool exists(std::string_view name) const;
  std::filesystem::path path(std::string_view name) const;
};
```

`Lab` gains `ConditionalFiles condition_files`. `Lab::metric_catalog()`
already exists and is used as is.

## 5. Editor window

`apps/pychron-ui/src/conditionals_editor_window.{hpp,cpp}`,
`conditional_table_model.{hpp,cpp}`, `conditional_form.{hpp,cpp}`.

```
+- files ----+- system.toml -------------------------------+
| system     | Kind         Name       Check       Action  |
| queue_ar   | truncation   big        Ar40 > 8e5  truncate|
| run_cdd    | termination  neg36      average(..) -       |
| [+] [-]    +---------------------------------------------+
|            | Name  [big      ]   Kind [truncation v]     |
|            | Check [Ar40 > 8e5                     ] ok  |
|            | Start [20] Freq [1] Ntrips [1] Win [  ]     |
|            | Mapper [        ]                           |
|            | Types [x]unknown [ ]blank [ ]air ...        |
|            | Ratio [0.5]  Action [truncate v] [ ]quick   |
|            +---------------------------------------------+
|            | Disable: system:gauge_high  [+] [-]         |
|            | Diagnostics                                 |
+------------+---------------------------------------------+
```

### 5.1 ConditionalTableModel

A `QAbstractTableModel` over one `ConditionalSet`. Columns Kind, Name, Check,
Action (read-only cells; editing is in the form). Operations: `add(kind)`,
`remove(row)`, `duplicate(row)`, `move_up/down(row)`, `replace(row,
Conditional)`, `set_disable(list)`. Each row carries its `finalize` error, if
any; such rows are drawn in the error colour with the message as tooltip.
`modified()` compares against the set last loaded or saved. Signal
`changed()`.

Rows are shown grouped by kind in the writer's order, so the table matches
the file that will be written; move up/down acts within a kind.

### 5.2 ConditionalForm

A widget editing one `Conditional`; emits `edited(Conditional)` on every
change. Built from `fields_of(kind)`:

- Name (placeholder: the default name), Kind combo.
- Check: single line, monospace. Compiled on every change; the error text
  appears under the field.
- Start, Frequency (hidden when `!gating`), Ntrips, Window (empty = none),
  Mapper.
- Analysis types: checkable list of the lab's analysis types plus `blank`;
  none ticked = all.
- Ratio (when `ratio`).
- Action: a combo of `actions` (hidden when empty) and the parameters of the
  chosen type: `quick` for truncate; name for `run_hook`; name and value for
  `set_param`; N for `skip_n`; a comma-separated steps field and a percent
  box for `set_extract`. The widgets build an `ActionSpec` directly.
- Resume (when `resume`); Truncate / Terminate as a three-way choice (none,
  truncate, terminate) when `run_flags`.

Changing Kind keeps the common fields, resets the action to the new kind's
default and clears fields the new kind does not take.

### 5.3 Window

- Left: the lab's files (`ConditionalFiles::list`), `system` first. `+` asks
  for a name and creates an empty set in memory (written on first save); `-`
  deletes the file after a confirmation; the confirmation says so when the
  queue open in the experiment window references the file.
- One file open at a time. Switching files or closing with unsaved changes
  asks Save / Discard / Cancel (the script editor's `Unsaved` pattern, with
  the same test hook).
- Disable list: plain strings, add and remove.
- Diagnostics: `finalize` errors immediately; `validate_conditionals(set,
  lab.metric_catalog())` 600 ms after the last change. Clicking a line
  selects the row.
- Save (Ctrl+S): refused with a message while any row has a `finalize`
  error (D4); otherwise `to_toml` then `ConditionalFiles::write`. A file that
  fails to parse on open is shown with the parser's message and is not
  editable (fix it by hand or delete it).
- Comment warning (D2): on open, the text is scanned for `#` outside strings;
  if any, the first save asks for confirmation.
- Emits `saved(name)`; the experiment window revalidates its queue on it.
- Geometry and the last open file are kept in `QSettings` like the script
  editor.

## 6. Assignment

### 6.1 QueueTableModel

- New column `Conditionals` after `Plan`: the run's reference names joined
  with ", ". Read-only cell.
- `bool set_conditionals(std::vector<std::size_t> rows, const
  std::vector<std::string>& names)`: replaces the references of each row;
  a name the row already references keeps its `kind`, a new one gets the
  default. Follows `row_editable`; false and nothing changes when any row is
  frozen.
- `bool set_queue_conditionals(const std::string& name)`: false while live
  (D6).

Both go through the existing edit path, so the queue is revalidated and
`edited()` is emitted.

### 6.2 ExperimentWindow

- A "Queue conditionals" combo above the table: `(none)` plus the lab's
  files; shows a missing name in the error colour rather than dropping it.
  An "Edit..." button opens the editor on the chosen file.
- Context menu on selected rows: "Set conditionals...", a dialog with the
  lab's files as a checkable list (tri-state when the rows differ; a file left
  half-ticked stays as each row has it) and an "Edit..." button.
- Scripts menu, beside the script editor: "Conditionals Editor...".
- One editor window per experiment window, created on first use.

### 6.3 RunFactoryPanel

A "Conditionals" checkable drop-down in the measurement group; its selection
goes into `FactoryForm` and from there into the runs Add inserts.
`FactoryForm` gains `std::vector<std::string> conditionals`.

## 7. Errors

| Situation | Behaviour |
|---|---|
| Directory missing | Empty file list; the first save creates it. |
| File does not parse | Listed; opening shows the message, no editing. |
| Write fails | Message box with the OS error; the document stays modified. |
| Bad file name on create | Refused with the rule (plain name). |
| Name exists on create | Refused, including a name that differs only by case on a case-insensitive filesystem. |
| Duplicate conditional names in a file | The table model marks the later row with an error; blocks save like a `finalize` error. |
| Referenced file deleted | The queue's validation reports it, as today. |

## 8. Testing

- `tests/experiment/test_conditionals_toml.cpp`: `to_toml` round trip for
  every kind, every field and every action type; defaults omitted; string
  escaping; `configs/examples/conditionals/*`,
  `profiles/instrument-common/conditionals/*` and the importer's expected
  outputs survive parse -> write -> parse unchanged; write is idempotent
  (`to_toml(parse(to_toml(s))) == to_toml(s)`).
- `tests/experiment/test_conditionals.cpp`: `fields_of` and `finalize` agree
  with the parser for each kind (the existing parser tests keep passing after
  the rewrite).
- `tests/experiment/test_conditional_files.cpp`: list, read, write, remove,
  missing directory, bad names, no partial file after a failed write.
- `tests/ui/test_conditionals_editor.cpp`: open the example lab; add, edit,
  duplicate, move, remove; kind change hides and resets fields; a bad check
  shows its error and blocks save; diagnostics appear after the delay and
  select the row; unsaved prompt on switch and close; save writes the
  expected text; comment warning; unparsable file.
- `tests/ui/test_queue_table_model.cpp`: the column, `set_conditionals` (kind
  kept, frozen rows refused), `set_queue_conditionals` (refused live).
- `tests/ui/test_experiment_window.cpp`, `test_run_factory_panel.cpp`: the
  combo and dialog set the specs; Add carries the panel's selection; a save
  in the editor revalidates the queue.
