# Conditionals Editor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A structured editor for `<lab>/conditionals/*.toml` in `pychron-ui`, plus pickers that attach those files to a queue and to runs.

**Architecture:** Qt-free pieces go in `libs/experiment/conditionals`: a canonical TOML writer, the per-kind field rules as data (`fields_of`, `finalize`) shared with the parser, and `ConditionalFiles` for the directory. `apps/pychron-ui` adds a table model, a form widget and an editor window over a `ConditionalSet`, and the experiment window gets a queue combo, a run dialog, a table column and a factory field.

**Tech Stack:** C++20, toml++ (already used by the parser), Qt 6 Widgets, GoogleTest (+ QtTest in `tests/ui`), CMake presets `dev` and `dev-ui`.

**Spec:** `docs/superpowers/specs/2026-10-03-conditionals-editor-design.md` (read it; section numbers below refer to it). Model reference: `docs/superpowers/specs/2026-10-02-conditionals-design.md`.

## Global Constraints

- Work on branch `conditionals-editor`. No pull requests; merge to `main` when done (AGENTS.md).
- No Qt in `libs/experiment` (headers or sources).
- Never skip or disable a failing test.
- Tests are globbed: `tests/experiment/test_*.cpp` needs no CMake edit. `tests/ui` and `apps/pychron-ui/CMakeLists.txt` list files explicitly: add new sources next to `script_editor_window.*` / `test_script_editor.cpp`.
- Build and run core tests: `cmake --build --preset dev && ctest --preset dev -R <pattern>`. UI: `cmake --build --preset dev-ui && ctest --preset dev-ui -R <pattern>`.
- Lifetime: the editor and panels take `const experiment::lab::Lab&`; the lab outlives them. In tests declare the lab before the widget.
- The parser's existing error messages must not change (existing tests assert on them).
- Validation delay is 600 ms (`kCheckDelayMs`, as in `ScriptEditorWindow`).
- Commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. A check or name containing `"`, `\` or a newline: saved file must parse back to the same string (Task 1 test `EscapesStrings`).
2. A file with two conditionals whose default names collide (same kind, same check, no `name`): must be flagged and block save, not silently drop one on merge (Task 3 test `DuplicateNamesAreErrors`).
3. Doubles such as `5e-9`, `0.1`, `8e5`: written so they parse back bit-equal, and never as an integer-looking token that toml++ would read as an int for a double key (Task 1 test `DoublesRoundTrip`).
4. A run that already references a file no longer in the lab: the dialog and column must keep showing it and "OK without changes" must not drop it (Task 5 test `MissingReferenceKept`).
5. Save when the file changed or vanished on disk, or the directory is read-only: document stays modified and the error is shown; no partial file (Task 2 test `FailedWriteLeavesNoPartial`, Task 4 test `WriteFailureKeepsModified`).

---

## File structure

| File | Responsibility |
|---|---|
| `libs/experiment/include/pychron/experiment/conditionals/conditional.hpp` (modify) | declare `to_toml`, `KindFields`, `fields_of`, `finalize`, `default_name` |
| `libs/experiment/src/conditionals/conditional_toml.cpp` (create) | `to_toml` |
| `libs/experiment/src/conditionals/conditional.cpp` (modify) | `fields_of`, `finalize`; parser uses them |
| `libs/experiment/include/pychron/experiment/conditionals/library.hpp`, `src/conditionals/library.cpp` (modify) | `ConditionalFiles` |
| `libs/experiment/include/pychron/experiment/lab/lab.hpp`, `src/lab/lab.cpp` (modify) | `Lab::condition_files` |
| `apps/pychron-ui/src/conditional_table_model.{hpp,cpp}` (create) | model over one `ConditionalSet` |
| `apps/pychron-ui/src/conditional_form.{hpp,cpp}` (create) | form for one `Conditional` |
| `apps/pychron-ui/src/conditionals_editor_window.{hpp,cpp}` (create) | file list, table, form, disable list, diagnostics, save |
| `apps/pychron-ui/src/queue_table_model.{hpp,cpp}` (modify) | column, `set_conditionals`, `set_queue_conditionals` |
| `apps/pychron-ui/src/experiment_window.{hpp,cpp}` (modify) | combo, dialog, menu, editor ownership |
| `apps/pychron-ui/src/run_factory_panel.{hpp,cpp}`, `libs/experiment/.../factory/form.{hpp,cpp}` (modify) | `FactoryForm::conditionals` and its widget |

---

### Task 1: Kind rules as data, and the TOML writer

**Files:**
- Modify: `libs/experiment/include/pychron/experiment/conditionals/conditional.hpp`
- Modify: `libs/experiment/src/conditionals/conditional.cpp` (the per-kind block around lines 470-520 and the default-name code near line 447)
- Create: `libs/experiment/src/conditionals/conditional_toml.cpp`
- Test: `tests/experiment/test_conditionals_toml.cpp` (create), `tests/experiment/test_conditionals.cpp` (add)

**Interfaces:**
- Produces (namespace `pychron::experiment`):

```cpp
struct KindFields {
  bool gating = false;     // start, frequency
  bool ratio = false;      // abbreviated_count_ratio
  bool resume = false;
  bool run_flags = false;  // truncate / terminate
  std::vector<ActionSpec::Type> actions;  // allowed; empty = no `action` key
  ActionSpec::Type default_action = ActionSpec::Type::None;
};
const KindFields& fields_of(ConditionalKind k);
std::string_view table_name(ConditionalKind k) noexcept;      // "truncations", "pre_run", ...
std::string default_name(ConditionalKind k, std::string_view check);  // what the parser assigns
// Compiles check/window/mapper into c.expr, fills an empty name with
// default_name, an action of Type::None with the kind's default, and applies
// the per-kind rules. Errors are the parser's messages.
Result<Conditional> finalize(Conditional c);
std::string to_toml(const ConditionalSet& set);
// Kinds in the order to_toml writes them.
inline constexpr ConditionalKind kFileOrder[] = {Truncation, Termination, Cancelation, Action,
                                                 Modification, Equilibration, PreRun, PostRun};  // qualified in code
```

`fields_of` values (from the parser at `conditional.cpp:474-516` and the 10-02 spec):

| Kind | gating | ratio | resume | run_flags | actions | default |
|---|---|---|---|---|---|---|
| Truncation | y | y | | | Truncate | Truncate |
| Termination | y | | | | (none) | Terminate |
| Cancelation | y | | | | (none) | Cancel |
| Action | y | | y | | Truncate, Terminate, Cancel, SetParam, RunHook, Notify | None |
| Modification | y | y | | y | the seven queue actions | SkipNext |
| Equilibration | y | y | | | (none) | None |
| PreRun | | | | | Cancel | Cancel |
| PostRun | | | | | Cancel + the seven queue actions | Cancel |

Before filling the table, read the parser block: if it accepts `abbreviated_count_ratio` or `start`/`frequency` on a kind the table says it should not, keep the parser's behaviour (do not add new rejections) and set the `KindFields` flag to match what the *spec* table says the form should show. `finalize` must reject exactly what the parser rejects today.

- [ ] **Step 1: Failing tests for the rules** in `test_conditionals.cpp`:

```cpp
TEST(ConditionalKinds, FieldsOf) {
  EXPECT_TRUE(fields_of(ConditionalKind::Truncation).ratio);
  EXPECT_TRUE(fields_of(ConditionalKind::Action).resume);
  EXPECT_TRUE(fields_of(ConditionalKind::Modification).run_flags);
  EXPECT_TRUE(fields_of(ConditionalKind::Termination).actions.empty());
  EXPECT_FALSE(fields_of(ConditionalKind::PreRun).gating);
  EXPECT_EQ(fields_of(ConditionalKind::Modification).default_action, ActionSpec::Type::SkipNext);
}
TEST(ConditionalKinds, FinalizeMatchesParser) {
  Conditional c; c.kind = ConditionalKind::Action; c.check = "Ar40 > 1";
  auto r = finalize(c);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().what, "an action conditional needs 'action'");
  c.kind = ConditionalKind::Truncation;
  auto ok = finalize(c);
  ASSERT_TRUE(ok);
  EXPECT_EQ(ok->name, default_name(ConditionalKind::Truncation, "Ar40 > 1"));
  EXPECT_EQ(ok->action.type, ActionSpec::Type::Truncate);
  ASSERT_TRUE(ok->expr);
  c.resume = true;
  EXPECT_EQ(finalize(c).error().what, "'resume' applies to actions only");
  c = {}; c.kind = ConditionalKind::Modification; c.check = "Ar40 < 1"; c.truncate = c.terminate = true;
  EXPECT_EQ(finalize(c).error().what, "a modification may truncate or terminate, not both");
  c = {}; c.check = "Ar40 >";
  EXPECT_FALSE(finalize(c));
}
```

- [ ] **Step 2:** `cmake --build --preset dev && ctest --preset dev -R ConditionalKinds` — fails to compile.
- [ ] **Step 3:** Implement `fields_of`, `table_name`, `default_name`, `finalize` in `conditional.cpp`; rewrite the parser's per-kind block to fill a `Conditional` from the table and call `finalize` (keep the one check that needs the table itself: `'action' is not allowed on <kind>s` when `t.contains("action")`).
- [ ] **Step 4:** `ctest --preset dev -R "Conditional"` — all pass, including every pre-existing conditionals test.
- [ ] **Step 5:** Commit `conditionals: per-kind field rules as data; finalize()`.

- [ ] **Step 6: Failing writer tests** in `test_conditionals_toml.cpp`. Helper `same(a, b)`: equal `disable`, and per item equal `name, kind, check, start, frequency, ntrips, window, mapper, analysis_types, abbreviated_count_ratio, action, resume, truncate, terminate`.

```cpp
TEST(ConditionalsToml, DefaultsOmitted) {
  auto s = *parse_conditionals("[[truncations]]\ncheck = \"Ar40 > 8e5\"\n");
  EXPECT_EQ(to_toml(s), "[[truncations]]\ncheck = \"Ar40 > 8e5\"\n");
}
TEST(ConditionalsToml, CanonicalOrder) {  // disable first, kinds in kFileOrder, keys in spec 4.1 order
  auto s = *parse_conditionals(R"(
disable = ["system:gauge_high"]
[[post_run]]
action = "run_blank"
check = "Ar40 < $MIN"
analysis_types = ["air", "cocktail"]
[[truncations]]
abbreviated_count_ratio = 0.5
start = 20
check = "Ar40 > 8e5"
name = "big"
)");
  EXPECT_EQ(to_toml(s),
            "disable = [\"system:gauge_high\"]\n\n"
            "[[truncations]]\nname = \"big\"\ncheck = \"Ar40 > 8e5\"\nstart = 20\nabbreviated_count_ratio = 0.5\n\n"
            "[[post_run]]\ncheck = \"Ar40 < $MIN\"\nanalysis_types = [\"air\", \"cocktail\"]\naction = \"run_blank\"\n");
}
TEST(ConditionalsToml, EveryFieldRoundTrips);   // one conditional per kind with every field that kind takes non-default,
                                                // every ActionSpec::Type incl. truncate:quick, set_param X=1.5, skip_n 3,
                                                // set_extract 1,2,3 and 10%,20%; window = 10; mapper = "x + 1000"
TEST(ConditionalsToml, EscapesStrings);         // name "a\"b\\c", check containing a quoted string if the grammar allows one,
                                                // disable entry with a quote: same(parse(to_toml(s)), s)
TEST(ConditionalsToml, DoublesRoundTrip);       // ratios 0.1, 0.25, 1e-3; set_param values 5e-9, 8e5, 3.0: bit-equal after round trip
TEST(ConditionalsToml, Idempotent);             // to_toml(parse(to_toml(s))) == to_toml(s)
TEST(ConditionalsToml, LabFilesSurvive);        // every *.toml under PYCHRON_EXAMPLE_CONFIGS_DIR/conditionals and
                                                // profiles/instrument-common/conditionals: same(parse(to_toml(parse(text))), parse(text))
TEST(ConditionalsToml, EmptySet) { EXPECT_EQ(to_toml(ConditionalSet{}), ""); }
```

Use the path macro the other experiment tests use for `configs/examples` (grep `PYCHRON_EXAMPLE` in `tests/experiment`); add one for `profiles/` in `tests/experiment/CMakeLists.txt` if none exists.

- [ ] **Step 7:** Build; tests fail to link (`to_toml` undefined).
- [ ] **Step 8:** Implement `to_toml` in `conditional_toml.cpp` by string building (not through toml++'s formatter, so order and omission are ours). Strings as TOML basic strings (escape `\`, `"`, control characters). Doubles with `std::to_chars` shortest form; append `.0` when the result has no `.`, `e`, `inf` or `nan`. `action` written as `to_string(ActionSpec)` unless it equals a default-constructed action of the kind's default type, and never when `fields_of(kind).actions` is empty. `name` omitted when equal to `default_name(kind, check)`. Blocks separated by one blank line; file ends with one newline.
- [ ] **Step 9:** `ctest --preset dev -R "ConditionalsToml|Conditional"` — pass.
- [ ] **Step 10:** Commit `conditionals: canonical TOML writer`.

---

### Task 2: ConditionalFiles

**Files:**
- Modify: `libs/experiment/include/pychron/experiment/conditionals/library.hpp`, `libs/experiment/src/conditionals/library.cpp`
- Modify: `libs/experiment/include/pychron/experiment/lab/lab.hpp`, `libs/experiment/src/lab/lab.cpp:112`
- Test: `tests/experiment/test_conditional_files.cpp` (create)

**Interfaces:**
- Produces:

```cpp
class ConditionalFiles {
 public:
  explicit ConditionalFiles(std::filesystem::path dir);
  Result<std::vector<std::string>> list() const;   // names without ".toml", sorted, "system" first; {} when dir missing
  Result<std::string> read(std::string_view name) const;                    // Config error when missing
  Result<void> write(std::string_view name, std::string_view text) const;   // creates dir; temp file in dir + rename
  Result<void> remove(std::string_view name) const;
  bool exists(std::string_view name) const;
  std::filesystem::path path(std::string_view name) const;
  static bool valid_name(std::string_view name);   // non-empty, no '/', '\\', "..", no leading '.', no ".toml" suffix
};
// Lab:
std::unique_ptr<ConditionalFiles> condition_files;   // <lab>/conditionals, set in load_lab
```

Reuse the plain-name check `DirectoryConditionalSource` already has (move it to `valid_name` and call it from both). Use whatever `Result<void>` spelling the library already uses.

- [ ] **Step 1: Failing tests** (each in a fresh temp directory):

```cpp
TEST(ConditionalFiles, MissingDirectoryListsNothing);          // list() ok and empty; exists("system") false
TEST(ConditionalFiles, WriteCreatesDirectoryAndLists);         // write("b",..), write("system",..), write("a",..) -> list == {"system","a","b"}
TEST(ConditionalFiles, ReadBack);                              // read == written text; read("nope") is an error naming the path
TEST(ConditionalFiles, RemoveDeletes);                         // then exists false; remove of a missing file is an error
TEST(ConditionalFiles, BadNamesRefused);                       // "", "a/b", "..", "../x", ".hidden", "x.toml" -> write/read/remove error, nothing created
TEST(ConditionalFiles, IgnoresOtherFiles);                     // "notes.txt", a subdirectory, "x.toml.tmp" not listed
TEST(ConditionalFiles, FailedWriteLeavesNoPartial);            // make dir read-only (skip on Windows / when running as root): write errors,
                                                               // directory listing unchanged, existing file content unchanged
TEST(ConditionalFiles, LabHasThem);                            // load_lab(example configs): condition_files->list() contains "system" and "default_unknown"
```

- [ ] **Step 2:** Build — fails to compile.
- [ ] **Step 3:** Implement. Temp file name `.<name>.toml.tmp` in the same directory; remove it on any failure.
- [ ] **Step 4:** `ctest --preset dev -R "ConditionalFiles|Lab"` — pass.
- [ ] **Step 5:** Commit `conditionals: ConditionalFiles (list, read, atomic write, remove)`.

---

### Task 3: ConditionalTableModel and ConditionalForm

**Files:**
- Create: `apps/pychron-ui/src/conditional_table_model.{hpp,cpp}`, `apps/pychron-ui/src/conditional_form.{hpp,cpp}`
- Modify: `apps/pychron-ui/CMakeLists.txt`, `tests/ui/CMakeLists.txt`
- Test: `tests/ui/test_conditionals_editor.cpp` (create; Task 4 adds to it)

**Interfaces:**
- Consumes: `fields_of`, `finalize`, `default_name`, `kFileOrder`, `to_string(ActionSpec)` (Task 1).
- Produces (namespace `pychron::ui`):

```cpp
class ConditionalTableModel : public QAbstractTableModel {
  Q_OBJECT
 public:
  enum Column { Kind, Name, Check, Action, Count };
  explicit ConditionalTableModel(QObject* parent = nullptr);
  void set(experiment::ConditionalSet set);            // sorts by kFileOrder (stable); becomes the clean state
  const experiment::ConditionalSet& conditionals() const;  // items as authored (name may be empty = default)
  void mark_clean();
  bool modified() const;
  int add(experiment::ConditionalKind kind);           // appended at the end of its kind; returns the row
  bool remove(int row);
  int duplicate(int row);                              // below `row`; name gets "_copy" when it was set
  bool move_up(int row);                               // within its kind only
  bool move_down(int row);
  bool replace(int row, experiment::Conditional c);    // re-sorts when the kind changed; returns via rowOf
  int row_of(const QString& effective_name) const;     // -1 when absent
  void set_disable(QStringList names);
  QStringList disable() const;
  QString error(int row) const;                        // finalize error or "duplicate name '<n>'"; empty when fine
  bool has_errors() const;
 signals:
  void changed();
};

class ConditionalForm : public QWidget {
  Q_OBJECT
 public:
  explicit ConditionalForm(QStringList analysis_types, QWidget* parent = nullptr);
  void set_conditional(const experiment::Conditional& c);   // does not emit edited
  experiment::Conditional conditional() const;
  QString check_error() const;                               // text shown under the check field
  // For tests: visible-state of the kind-dependent groups.
  bool shows_gating() const, shows_ratio() const, shows_action() const, shows_resume() const, shows_run_flags() const;
 signals:
  void edited(const experiment::Conditional& c);
};
```

Widgets carry `objectName`s the tests use: `name`, `kind`, `check`, `start`, `frequency`, `ntrips`, `window`, `mapper`, `types`, `ratio`, `action`, `action_quick`, `action_name`, `action_value`, `action_count`, `action_steps`, `action_percent`, `resume`, `run_flag` (combo: none / truncate / terminate).

Decisions:
- A new row's check is empty, so it has an error until edited (it blocks save, which is intended).
- Effective name = `name` if set, else `default_name(kind, check)`. Duplicate effective names: the later row carries the error.
- Display: Kind = singular word; Name = effective name (grey when defaulted); Check = authored check; Action = `to_string(action)` or "-" when the kind has none. Error rows: `Qt::ForegroundRole` error colour (reuse the colour `QueueTableModel` uses for errors), `Qt::ToolTipRole` = `error(row)`.
- Form on kind change: keep name, check, ntrips, window, mapper, analysis types; reset action to `fields_of(new).default_action`; clear start/frequency when `!gating`, ratio to 1.0 when `!ratio`, resume and run flags when not applicable.
- Window spin: 0 shown as blank = `nullopt`. `set_extract` steps field parses with `parse_action("set_extract " + text)`; an unparsable field leaves the previous `ActionSpec` and shows the error under the action row.
- Analysis types passed in are the lab's (`to_string` of each `AnalysisType` the project defines, lowercased) plus `"blank"`; types present on a conditional but not in the list are added as extra checked entries rather than dropped.

- [ ] **Step 1: Failing model tests:**

```cpp
TEST(ConditionalTableModel, SortsByFileOrder);          // set({post_run, truncation, action}) -> rows truncation, action, post_run
TEST(ConditionalTableModel, AddRemoveDuplicateMove);    // add(Truncation) lands after the last truncation; move across kinds refused;
                                                        // changed() emitted once per operation; modified() true, false after mark_clean()
TEST(ConditionalTableModel, ReplaceResortsOnKindChange);
TEST(ConditionalTableModel, ErrorsPerRow);              // check "Ar40 >" -> error non-empty, has_errors; fixed -> empty
TEST(ConditionalTableModel, DuplicateNamesAreErrors);   // two truncations "Ar40 > 1" without names: row 1 error == "duplicate name 'truncation:Ar40 > 1'"
                                                        // (use default_name() for the expected text); naming one clears it
TEST(ConditionalTableModel, ModifiedComparesContent);   // edit then edit back -> modified() false
TEST(ConditionalTableModel, DisableList);
```

- [ ] **Step 2: Failing form tests:**

```cpp
TEST(ConditionalForm, KindShowsFields);        // truncation: gating, ratio, action(quick only) ; termination: no action ;
                                               // action: resume ; modification: run_flag, ratio ; pre_run: no gating
TEST(ConditionalForm, RoundTripsEveryField);   // set_conditional(c) then conditional() == c for one conditional per kind with all fields set
TEST(ConditionalForm, EditEmits);              // typing in `check` emits edited with the new check; set_conditional emits nothing
TEST(ConditionalForm, BadCheckShowsError);     // "Ar40 >" -> check_error() non-empty; "Ar40 > 1" -> empty
TEST(ConditionalForm, KindChangeResets);       // action+resume+hook -> truncation: resume false, action Truncate, name/check kept
TEST(ConditionalForm, ActionParameters);       // choose set_param, name "X", value 1.5 -> ActionSpec{SetParam,"X",1.5};
                                               // skip_n 3; set_extract "10%,20%" -> steps {10,20}, percent true
TEST(ConditionalForm, UnknownAnalysisTypeKept);// conditional with analysis_types {"weird"} round-trips
```

- [ ] **Step 3:** `cmake --build --preset dev-ui` — fails to compile.
- [ ] **Step 4:** Implement both classes.
- [ ] **Step 5:** `ctest --preset dev-ui -R "ConditionalTableModel|ConditionalForm"` — pass.
- [ ] **Step 6:** Commit `ui: conditional table model and form`.

---

### Task 4: ConditionalsEditorWindow

**Files:**
- Create: `apps/pychron-ui/src/conditionals_editor_window.{hpp,cpp}`
- Modify: `apps/pychron-ui/CMakeLists.txt`
- Test: `tests/ui/test_conditionals_editor.cpp`

**Interfaces:**
- Consumes: Task 3 classes; `Lab::condition_files`, `Lab::metric_catalog()`, `parse_conditionals`, `to_toml`, `validate_conditionals`.
- Produces:

```cpp
class ConditionalsEditorWindow : public QMainWindow {
  Q_OBJECT
 public:
  enum class Unsaved { Save, Discard, Cancel };
  static constexpr int kCheckDelayMs = 600;
  explicit ConditionalsEditorWindow(const experiment::lab::Lab& lab, std::unique_ptr<QSettings> settings = nullptr,
                                    QWidget* parent = nullptr);
  QStringList file_names() const;
  bool open(const QString& name);                           // false on Cancel of the unsaved prompt or unreadable file
  bool new_file(const QString& name, QString* error = nullptr);   // in memory until saved
  bool delete_file(const QString& name, QString* error = nullptr);
  QString current_name() const;
  bool editable() const;                                    // false for a file that did not parse
  QString load_error() const;
  ConditionalTableModel& model();
  ConditionalForm* form() const;
  void select_row(int row);
  int current_row() const;
  bool modified() const;
  bool save(QString* error = nullptr);
  void check_now();
  QStringList diagnostic_lines() const;                     // "error: big: ..." / "warning: ..."
  QString status_text() const;
  void set_ask_unsaved(std::function<Unsaved(const QString& name)> ask);
  void set_confirm(std::function<bool(const QString& question)> confirm);   // delete, comment loss
  // Whether the open queue references `name` (delete confirmation text).
  void set_referenced(std::function<bool(const QString& name)> referenced);
 signals:
  void saved(const QString& name);
  void filesChanged();
 protected:
  void closeEvent(QCloseEvent* event) override;
};
```

Decisions:
- Follow `ScriptEditorWindow` for settings keys (`conditionals_editor/geometry`, `conditionals_editor/last`), the unsaved hook and the debounce timer.
- Save refusal text: `"Fix the errors before saving (<n> conditional(s))"`.
- Comment detection: a `#` outside a TOML string in the loaded text. Confirmation text: `"<name>.toml has comments; saving rewrites the file and drops them. Save?"`. Asked once per file per window; declined = no write, still modified.
- Status text after a save: `"Saved <name>. Applies from the next queue start."` when name is `system`, else `"Saved <name>. Applies from the next run (next queue start when used as queue conditionals)."`.
- Diagnostics list = model errors (immediately) + `validate_conditionals` results (after the delay, run on the rows that finalize; skipped rows are already listed). Catalog errors do not block save (spec D4).
- A new file is listed with a trailing `*` and exists only in memory until saved; Discard on it removes it from the list.

- [ ] **Step 1: Failing tests** (fixture: `scratch_lab()` from `tests/ui/experiment_fixture.hpp`, `load_lab(lab_paths(dir))`, a temp `QSettings`; hooks default to Discard / true):

```cpp
TEST_F(ConditionalsEditor, ListsAndOpensSystemFirst);      // file_names()[0] == "system"; opened on construction; rows > 0
TEST_F(ConditionalsEditor, AddEditSave);                   // add truncation, set check "Ar40 > 8e5", start 20 through the form's widgets;
                                                           // save() true; file text parses; contains a truncation with start 20; saved("system") emitted;
                                                           // modified() false
TEST_F(ConditionalsEditor, SuccessCriterion);              // spec section 1: saved file passes parse_conditionals and has no error from
                                                           // validate_conditionals(lab.metric_catalog())
TEST_F(ConditionalsEditor, BadCheckBlocksSave);            // check "Ar40 >" -> save false, error mentions "Fix the errors", file unchanged on disk
TEST_F(ConditionalsEditor, CatalogErrorDoesNotBlock);      // check "Xx99 > 1": check_now(); diagnostic_lines() has an error line; save() true
TEST_F(ConditionalsEditor, DiagnosticsAfterDelay);         // edit; QTest::qWait(kCheckDelayMs + 200); diagnostic_lines() updated without check_now()
TEST_F(ConditionalsEditor, DiagnosticSelectsRow);          // activate the diagnostics item -> current_row() is that conditional
TEST_F(ConditionalsEditor, UnsavedPromptOnSwitchAndClose); // Cancel: open() false, still on the file, modified; Save: written, switched; Discard: not written
TEST_F(ConditionalsEditor, NewAndDelete);                  // new_file("run_x") listed; bad name / existing name refused with error; save creates
                                                           // <lab>/conditionals/run_x.toml; delete_file removes it; confirm declined keeps it
TEST_F(ConditionalsEditor, CommentWarningOnce);            // write a file with "# note"; open; edit; save with confirm=false -> not written;
                                                           // confirm=true -> written, confirm called once; second save does not ask
TEST_F(ConditionalsEditor, UnparsableFileNotEditable);     // write "[[truncations]]\ncheck = 3\n": open true, editable() false, load_error() non-empty, save false
TEST_F(ConditionalsEditor, WriteFailureKeepsModified);     // remove write permission on the directory (skip where not enforceable): save false with error, modified() true
TEST_F(ConditionalsEditor, DisableListSaved);              // add "system:gauge_high" -> saved text starts with disable = ["system:gauge_high"]
TEST_F(ConditionalsEditor, RemembersLastFile);             // open default_unknown; destroy; new window with same settings opens default_unknown
```

- [ ] **Step 2:** Build — fails to compile.
- [ ] **Step 3:** Implement. Layout per spec section 5: `QSplitter` (file `QListWidget` with +/- buttons | right column: `QTableView` with add (menu of kinds) / remove / duplicate / up / down buttons, `ConditionalForm`, disable `QListWidget` with +/-, diagnostics `QListWidget`), status label in the status bar, Ctrl+S.
- [ ] **Step 4:** `ctest --preset dev-ui -R ConditionalsEditor` — pass.
- [ ] **Step 5:** Commit `ui: conditionals editor window`.

---

### Task 5: Queue table — column and assignment operations

**Files:**
- Modify: `apps/pychron-ui/src/queue_table_model.{hpp,cpp}`
- Test: `tests/ui/test_queue_table_model.cpp`

**Interfaces:**
- Produces:

```cpp
enum Column { Row, Status, Identifier, Aliquot, Step, Type, Position, Extract, Script, Plan, Conditionals, Comment, Estimate, Count };
// Replaces each row's references with `names` (in that order). A name the row
// already references keeps its ConditionalRef (kind included); a new one is
// ConditionalRef{name}. False, nothing changed, when locked or any row is not row_editable.
bool set_conditionals(std::vector<std::size_t> rows, const std::vector<std::string>& names);
// False while live or locked.
bool set_queue_conditionals(const std::string& name);
```

Both use the path `replace_run` uses (commit to the executor when live, revalidate, emit `edited()`). The new column is read-only (`flags` without `ItemIsEditable`); display = names joined with ", ". Inserting an enum value shifts `Comment` and `Estimate`: grep `QueueTableModel::Comment`, `::Estimate` and any integer column literals in `apps/pychron-ui/src` and `tests/ui` and fix them.

- [ ] **Step 1: Failing tests:**

```cpp
TEST_F(QueueTableModelTest, ConditionalsColumn);        // run with refs {a, b}: data(row, Conditionals) == "a, b"; header "Conditionals"; not editable
TEST_F(QueueTableModelTest, SetConditionals);           // rows {0,1}, names {"default_unknown"}: both runs reference it; edited() once; revalidated
TEST_F(QueueTableModelTest, SetConditionalsKeepsKind);  // row has {"x","truncate"}; set {"x","y"} -> {"x","truncate"}, {"y","action"}
TEST_F(QueueTableModelTest, MissingReferenceKept);      // row references "gone" (no file): set_conditionals(rows, {"gone"}) is a no-op that returns true
                                                        // and the column still shows "gone"
TEST_F(QueueTableModelTest, SetConditionalsRefusedWhenFrozen);  // set_live(1, commit): rows {0} false and unchanged; rows {1} true and commit called
TEST_F(QueueTableModelTest, SetQueueConditionals);      // sets QueueSpec::queue_conditionals, edited(); false while live; false when locked
```

- [ ] **Step 2:** Build — fails. **Step 3:** Implement. **Step 4:** `ctest --preset dev-ui -R "QueueTableModel|ExperimentWindow|RunFactory"` — all pass (column shift did not break others).
- [ ] **Step 5:** Commit `ui: conditionals column and assignment in the queue table model`.

---

### Task 6: Experiment window and run factory wiring

**Files:**
- Modify: `apps/pychron-ui/src/experiment_window.{hpp,cpp}`
- Modify: `libs/experiment/include/pychron/experiment/factory/form.hpp`, `libs/experiment/src/factory/form.cpp`
- Modify: `apps/pychron-ui/src/run_factory_panel.{hpp,cpp}`
- Test: `tests/experiment/test_factory_form.cpp` (the existing form test file; grep `build_runs` in `tests/experiment`), `tests/ui/test_run_factory_panel.cpp`, `tests/ui/test_experiment_window.cpp`

**Interfaces:**
- Consumes: Task 4 window, Task 5 model operations, `Lab::condition_files`.
- Produces:

```cpp
// FactoryForm
std::vector<std::string> conditionals;   // file names; build_runs copies them to every run as ConditionalRef{name};
                                         // form_from_run fills it from the run's reference names

// RunFactoryPanel
void refresh_conditionals();             // re-read the lab's files into the drop-down, keeping the ticks

// ExperimentWindow
ConditionalsEditorWindow* conditionals_editor() const noexcept;   // null until first opened
ConditionalsEditorWindow* open_conditionals_editor(const QString& file = {});
QComboBox* queue_conditionals_combo() const noexcept;
// Applies `names` to the selected rows; false without a selection or when the model refuses.
bool set_selected_conditionals(const std::vector<std::string>& names);
// Dialog hook: (file names, initial check states) -> chosen names, nullopt = cancelled. Tests replace it.
using PickConditionals = std::function<std::optional<std::vector<std::string>>(const QStringList&, const QList<Qt::CheckState>&)>;
void set_pick_conditionals(PickConditionals pick);
```

Decisions:
- Combo items: `(none)` then `condition_files->list()`. A `queue_conditionals` not in the list is added as its own item in the error colour. Disabled while the model is live. Kept in sync on `set_queue`/`on_queue_edited`.
- "Edit..." beside the combo: opens the editor on the chosen file (on `system` when none).
- Row context menu "Set conditionals...": initial states are Checked when every selected row references the name, Unchecked when none, PartiallyChecked otherwise; names referenced but not in the lab appear too (checked/partial). On accept, names left PartiallyChecked are left as each row had them (apply per row: checked names added, unchecked removed, partial untouched) — so call `set_conditionals` once per distinct resulting list.
- Experiment menu action "Conditionals Editor", next to the script editor action.
- Editor wiring: `saved` and `filesChanged` -> `model_.revalidate()`, refill the combo, `factory_->refresh_conditionals()`. `set_referenced` -> whether the open queue's `queue_conditionals` or any run references the name.
- Factory drop-down: a `QToolButton` with a checkable `QMenu` (objectName `conditionals`), text = ticked names joined with ", " or "(none)". In the measurement group.

- [ ] **Step 1: Failing tests:**

```cpp
// tests/experiment
TEST(FactoryForm, ConditionalsCopiedToRuns);            // form.conditionals {"default_unknown"}; build_runs with a 3-hole position -> every run has that ref
TEST(FactoryForm, FormFromRunReadsConditionals);
// tests/ui/test_run_factory_panel.cpp
TEST_F(RunFactoryPanelTest, ConditionalsDropDown);      // menu lists the lab's files; tick "default_unknown" -> form().conditionals; Add -> inserted run references it
// tests/ui/test_experiment_window.cpp
TEST_F(ExperimentWindowTest, QueueConditionalsCombo);   // items "(none)", "system", "default_unknown" (order per list()); choosing one sets
                                                        // model().queue().queue_conditionals and modified(); loading a queue that names one selects it
TEST_F(ExperimentWindowTest, UnknownQueueConditionalsShown);  // queue naming "gone": combo shows "gone" selected, not "(none)"
TEST_F(ExperimentWindowTest, SetConditionalsOnSelection);     // select rows 0-1, hook returns {"default_unknown"} -> both rows; hook returning nullopt changes nothing
TEST_F(ExperimentWindowTest, PickStatesTriState);             // row 0 has the ref, row 1 not: hook receives PartiallyChecked for it
TEST_F(ExperimentWindowTest, OpensConditionalsEditor);        // open_conditionals_editor("default_unknown")->current_name() == "default_unknown"; same instance on second call
TEST_F(ExperimentWindowTest, EditorSaveRevalidatesQueue);     // queue references run_x (missing) -> row error; create and save run_x in the editor -> row error gone,
                                                              // combo and factory menu list run_x
TEST_F(ExperimentWindowTest, ComboDisabledWhileLive);
```

- [ ] **Step 2:** Build both presets — fails. **Step 3:** Implement. **Step 4:** `ctest --preset dev` and `ctest --preset dev-ui` — everything passes.
- [ ] **Step 5:** Commit `ui: assign conditionals to queues and runs; editor in the experiment window`.

---

### Task 7: Documentation, full verification, merge

**Files:**
- Modify: `README.md` (pychron-ui row: add "conditionals editor"), `docs/superpowers/specs/2026-10-03-conditionals-editor-design.md` (Status: Implemented), `docs/superpowers/specs/2026-10-02-conditionals-design.md` (Status: Implemented; section 9: UI editors -> see the editor spec)

- [ ] **Step 1:** Edit the three files.
- [ ] **Step 2:** Clean verification: `cmake --build --preset dev && ctest --preset dev` and `cmake --build --preset dev-ui && ctest --preset dev-ui` — 100% pass, no new warnings.
- [ ] **Step 3:** Manual check of the spec's success line: `pychron-ui --sim --lab configs/examples configs/examples/experiment.toml` on a scratch copy of `configs/examples`; Experiment > Conditionals Editor; add truncation `Ar40 > 8e5`, start 20; save; `elctl exp conditionals check <file>` accepts it; pick it as queue conditionals; save the queue and confirm `queue_conditionals` in the file. Do not commit changes to `configs/examples`.
- [ ] **Step 4:** Commit `docs: conditionals editor implemented`.
- [ ] **Step 5:** `git fetch origin && git rebase origin/main`, rerun both test presets, merge `conditionals-editor` into `main` (fast-forward), push.
