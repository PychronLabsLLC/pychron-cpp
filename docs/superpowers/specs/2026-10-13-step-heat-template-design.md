# Step heat templates: named heating schedules and their editor

Status: draft 2026-10-13, not agreed. Written by unattended run for T022
("need a step heat template editor"). Nothing built. Questions for owner:
section 9.

## 1. Problem

Run factory makes a step heat from one text field: `5, 10, 15` or
`start:increment:count` (`libs/experiment/src/factory/form.cpp:30`
`parse_step_values`; `increments.cpp:118` `make_step_heat`). Every step gets
same duration and cleanup (the form's), only `extraction.value` and
`id.step` differ. Nothing is saved: a lab's usual schedule is typed again
each time, and a schedule whose steps differ in duration, cleanup or beam
diameter cannot be made at all except by editing rows one by one.

Legacy had named templates and an editor for them.

## 2. Legacy (read, not copied)

`pychron/experiment/queue/increment_heat_template.py`,
`pychron/experiment/automated_run/factory.py`.

- Files: `setupfiles/incremental_heat_templates/NN-<name>.txt`, NN = step
  count. CSV, header required, columns by name: `value`, `units` (`watts`,
  `temp`, `percent`), `duration`, `cleanup`; laser variant adds
  `beam_diameter`. Row order gives step letter (row 1 = A). Blank
  `duration`/`cleanup` = factory's global value. Value 0 rows kept on load,
  skipped on apply.
- Editor (`traits_view`, `:352`): template combo, add step (clones last or
  selected rows), global Duration, Cleanup, Units applied to selected rows
  (or all rows with a value when none selected); table Step, Value, Units,
  Duration, Cleanup [, Beam diameter]; Save (current file), Save As (name
  dialog). No delete row, no move.
- Use: factory combo "Step Heat Template". Add with template picked = one
  run per step per hole of position (`_render_template`, `:832`). Apply
  arrow = template applied to selected queue rows: row becomes step A, later
  steps inserted after it at same position (`do_apply_stepheat`, `:541`).
  Aliquot set to greatest + 1 per selected row.
- Per-user usage counts order the combo (pickle in hidden dir, `:859`).
- No ramps, increments or generators inside a template: explicit rows only.
- No sample template file in either legacy checkout or
  `~/Pychron/setupfiles`.

## 3. Goal

- A lab keeps named step heat templates as files in its lab directory,
  validated with the rest of the lab.
- Run factory takes a template by name; Add makes one run per step, each
  step's own value, duration, cleanup and laser fields.
- An editor window makes, edits, copies and deletes them.
- Text field stays, for one-off schedules; it can be saved as a template.

## 4. Files

### 4.1 Where

`<lab>/step_heats/<name>.toml`, beside `plans/`, `conditionals/`,
`patterns/` (lab setup, not queue files). Name = file stem; `[a-z0-9_-]`,
same rule as a block's name. Directory optional. Added to the layout list
in `libs/experiment` `lab/lab.hpp`. (Owner decided 2026-10-13 that blocks
move under `queues/blocks/`; templates are not run sequences, so not
there. Question 1.)

### 4.2 Format

```toml
[template]
name = "laser-12"            # optional; must equal the file stem when given
description = "CO2, 12 steps, sanidine"
units = "watts"              # default for steps: watts | temp | percent
duration = 30                # s, default for steps
cleanup = 15                 # s, default for steps

[[steps]]
value = 0.5

[[steps]]
value = 1.0
duration = 45

[[steps]]
value = 1.5
beam_diameter = 2.0
ramp_rate = 0.1
```

- `[[steps]]` in order; step letter from order (A, B, ..., `step_name`).
  1 to 1000 steps (`kMaxStepValues`, as the text field).
- Step keys: `value` (required, > 0), `units`, `duration`, `cleanup`,
  `pre_cleanup`, `post_cleanup`, `beam_diameter`, `ramp_rate`, `ramp`,
  `cryo_temp`: the `ExtractionSpec` fields that vary by step
  (`model/run_spec.hpp:20`). Same key names and units as a queue row's
  `[runs.extraction]` (`model/queue_toml.cpp`), read by the same code where
  it can be shared.
- A key absent from a step: the `[template]` default when it has one, else
  the form's (run's) value. Precedence: step, template, form.
- Not in a template: device, position, pattern, script, plan, identifier,
  aliquot. A template is a heating schedule, not a run.
- Unknown key, value <= 0, unit unknown, zero steps: `Config` error naming
  file and step (`step 3 (C): ...`), as `parse_block` does.
- Writer: canonical TOML, keys in the order above, a key equal to the
  `[template]` default left out. Hand comments lost on save (asked first,
  as the conditionals editor does). Files read back by pychron only: no
  `mark_as_user_file`.

## 5. Library (`libs/experiment`, Qt-free)

New `factory/step_heat_template.hpp`:

```cpp
struct StepHeatStep {
  double value = 0;
  std::optional<Unit> units;
  std::optional<Duration> duration, cleanup, pre_cleanup, post_cleanup, ramp;
  std::optional<double> beam_diameter, ramp_rate, cryo_temp;
  friend bool operator==(const StepHeatStep&, const StepHeatStep&) = default;
};

struct StepHeatTemplate {
  std::string name, description;
  std::optional<Unit> units;
  std::optional<Duration> duration, cleanup;
  std::vector<StepHeatStep> steps;
  friend bool operator==(const StepHeatTemplate&, const StepHeatTemplate&) = default;
};

Result<StepHeatTemplate> parse_step_heat(std::string_view text, std::string_view name);
Result<StepHeatTemplate> load_step_heat(const std::filesystem::path& path);
std::string to_toml(const StepHeatTemplate& t);

// One run per step, steps A, B, ...; tmpl's extraction overridden per 4.2.
std::vector<RunSpec> make_step_heat(const RunSpec& tmpl, const StepHeatTemplate& t);

// The text field's values as a template (units, duration, cleanup from the form).
StepHeatTemplate template_from_values(const std::vector<double>& values, const FactoryForm& form);
```

- Existing `make_step_heat(tmpl, values)` kept, written as
  `make_step_heat(tmpl, template_from_values(...))` so the two paths cannot
  drift.
- `Lab` gains `std::map<std::string, StepHeatTemplate> step_heats`, loaded
  with the lab; a file that does not parse is a lab diagnostic (as a block
  that does not parse), not a refusal of the lab.
- `FactoryForm` gains `std::string step_heat_template` (name). `build_runs`
  gets the lab's templates (new parameter or a context struct, plan
  decides): template named and step heat text both non-empty is a `Config`
  error; unknown name is an error; same "type must heat" rule as the text.
- Queue rows do not record which template made them: after Add they are
  ordinary rows. Changing a template never changes a queue already made.
- Units checked against extraction device as a typed value is today (no new
  rule). `percent` on a device that does not take it: same check as a row.

## 6. Run factory panel (`apps/pychron-ui` `run_factory_panel`)

- Step heat row becomes: combo of the lab's templates ("none" first, then by
  name) + the existing text field + Edit... button. Picking a template
  clears and disables the text; text typed clears the combo.
- Preview line, as today, says what Add makes: "12 runs, steps A to L,
  0.5 to 6 W, 21 min extraction".
- Save as template... (in the Edit menu of the editor and as a panel action
  when the text field has values): makes a template from the text and the
  form's units, duration, cleanup; opens it in the editor.
- `form_from_run` (From Row) does not guess a template from rows.
- Disabled for a type that does not heat, as the text is
  (`run_factory_panel.cpp:403`).

## 7. Editor window

`StepHeatEditorWindow`, `QMainWindow`, modelled on
`ConditionalsEditorWindow` (`conditionals_editor_window.hpp`): one file open
at a time.

- Left: the lab's templates (+ new, duplicate, - delete with confirm).
- Right, top: name (read-only; rename = save as), description, template
  defaults units, duration, cleanup.
- Right, table: Step (letter, read-only), Value, Units, Duration (s),
  Cleanup (s), Beam diameter, Ramp rate; empty cell = inherited, shown
  greyed with the inherited value. Multi-select.
- Row actions: add step (copies the selected or last step; value + the
  last increment when there are two or more steps), insert, delete, move
  up, move down; Fill... sets one column on selected rows (legacy's global
  fields, explicit). Fill from text...: `5:2.5:4` or a list replaces values
  of selected rows or appends.
- Totals under the table: step count, total extraction time (sum of
  `estimate_run` extraction parts, `run_spec.hpp:73`).
- Diagnostics: parse rules of 4.2 listed at once and keep the file from
  saving; Save, Save As, Revert. Unsaved changes asked about on close or on
  switching file.
- Opened from run factory's Edit... and from the experiment window's menu
  (beside Conditionals editor). Refreshes the factory's combo on save.
- Edits files only; allowed while a queue runs (a running queue already
  holds its rows).

## 8. Rules

- Precedence step > template > form is one function, used by Add and by the
  editor's greyed cells; tested once with a table.
- Template and text field never both feed one Add.
- Writer then reader gives an equal `StepHeatTemplate`; canonical file
  written twice is byte-identical.
- No file outside `<lab>/step_heats/` written by the editor.
- Rows made from a template are indistinguishable from rows made by hand:
  queue file format unchanged, `schema_version` unchanged.

## 9. Questions for owner

1. Location: `<lab>/step_heats/` (proposed) or under `queues/` beside
   blocks?
2. Apply to selected rows (legacy's arrow: selected row becomes A, steps
   inserted after it): wanted in first cut, or Add only?
3. Multi-hole position with a template: one schedule per hole (legacy) or
   refused as today (text field takes precedence over holes,
   `form.cpp:118`)?
4. Import legacy `incremental_heat_templates/*.txt` (`elctl exp
   import-step-heat`, or the lab setup's legacy import)? Owner refused a
   legacy queue `.txt` importer (queue format survey section 7); templates
   are small and a lab may have dozens.
5. Per-user ordering of the combo by use count (legacy): wanted? Proposed
   no; alphabetical.

## 10. Seen while reading, not part of this spec

`Run` (`libs/experiment/src/run/run.cpp:202`) asks `AliquotAllocator` for
each run on its own; with no fixed aliquot each step of a step heat looks
to get the next free aliquot (A as 5, B as 6), since
`persister.cpp:219` keeps the step and takes `next_aliquot` per call. Not
checked with a test. If so, every step heat made by the factory without a
fixed aliquot is split across aliquots today. Filed to the task Inbox; to be
confirmed before templates make more step heats.

## 11. Out of scope

- Ramps or generators inside a template (explicit steps only; the text
  field keeps `start:increment:count`).
- Conditionals that change a step heat while it runs (`set_extract`
  exists, `docs/user/04-experiments.md:613`).
- Furnace setpoint programs, per-step scripts or plans.
- Staged gas release within one run (T027,
  `2026-10-13-staged-gas-release-design.md`).
- `elctl` commands to edit templates; `elctl exp validate` checks them with
  the lab.

## 12. Tests

- `tests/experiment`: parse table (good file; each refusal of 4.2 with its
  message); round trip and byte-identical rewrite; precedence table;
  `make_step_heat` from template gives letters, values and per-step
  durations; text path through `template_from_values` gives today's runs
  (`test_run_factory.cpp` cases unchanged); `build_runs` refuses template
  plus text, unknown name, non-heating type; lab loads `step_heats/` and
  reports a bad file as a diagnostic.
- `tests/ui`: factory combo lists templates, picking one disables text,
  preview says count and time, Add inserts N rows; editor: add, insert,
  move, delete, fill, save, revert, a bad value blocks save; Save as
  template from text.
- Example lab: one template in `configs/examples` and the setup profile
  that renders it (`libs/setup/AGENTS.md` rules), checked by
  `tests/integration/test_install_e2e.cpp`.
