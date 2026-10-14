# Queue file format: experiment.toml vs legacy tab text (survey)

Survey only, for T041 ("is experiment.toml the best format; legacy used csv
with a special header; at minimum queue files stored in a subdirectory").
Input to brainstorm with owner. No design agreed; nothing built.

## 1. Legacy (Python pychron)

Not CSV: YAML-ish meta block, separator line, then **tab**-separated table.
Extension `.txt`; `.xls` open-only.

```
username: a
mass_spectrometer: Jan
delay_between_analyses: 20
extract_device: Fusions Diode
tray: ---
#================================================================================
labnumber	sample	position	e_value	e_units	duration	...	measurement	post_eq	post_meas	...
61312	NM-779				5.0	...
```

- Meta: `extract_meta` `pychron/experiment/queue/base_queue.py:73`, read until line starting `#=====`, parsed as YAML. Writer `METASTR` `:84`, `_meta_dumper` `:549`. Keys: `username`, `use_email`, `email`, `use_group_email`, `date` (written, never read), `queue_conditionals_name`, `mass_spectrometer`, `delay_before_analyses`, `delay_between_analyses`, `delay_after_blank`, `delay_after_air`, `delay_after_conditional`, `extract_device`, `default_lighting`, `tray`, `load`, `note`. `repository_identifier` read but not written. Unknown keys ignored.
- Table: `run_block.py:64` `_runs_gen`, `delim = "\t"`, first line header. Columns found by name, any order, any missing (default). `##` line = comment; `#` line = run with `skip=True`; blank line ignored.
- Columns written (`_get_dump_attrs`, `base_queue.py:507`): `labnumber, sample, position, e_value, e_units, duration, cleanup, preclean, postclean, cryo_temp, overlap, beam_diam, pattern, light_value, extraction, ramp, t_o, measurement, conditionals, syn_extraction, use_cdd_warming, post_meas, post_eq, s_opt, dis_btw_pos, weight, comment, autocenter, frequency_group, repository_identifier, delay_after`; UV adds `reprate, mask, attenuator, image`. Empty, zero, `NULL` written as empty.
- Scripts and conditionals by name (no path, no extension).
- No version key. Compatibility by column aliases (`parser.py:116`, `:119`, `_get_attr` tuples); writer still emits old short names. Bug: `parser.py:101` `if idx:` drops column 0.
- Run blocks: same table without meta, `.txt` under `experiments/blocks/`.
- Directories (`pychron/paths.py:360`–`369`, root `~/Pychron<name>`): `experiments/` (queues), `experiments/auto_save/` (`<name>.bak`, `experiment_queue.py:102`), `experiments/blocks/`, `experiments/rem/` (`<name>.rem.txt`, `<name>.ex.txt`: remaining and executed runs, `experiment_executor.py:1826`), `experiments/backup/`, `experiments/generic/` (defined, unused). Last open: `.appdata/last_experiment`.
- No real queue file in `~/pychron-dev/legacy` or `legacy-db`; only `test/data/experiment.txt`, `experiment2.txt` in legacy repo.

## 2. pychron-cpp (`experiment.toml`)

- Parse `parse_queue` `libs/experiment/src/model/queue_toml.cpp:281`; write `dump_queue` `libs/experiment/src/model/queue_file.cpp:187`; save `save_queue_file` `:221` (temp file + rename). Structs `run_spec.hpp` (`RunSpec` :45, `QueueSpec` :60). `kQueueSchemaVersion = 1` (`run_spec.hpp:69`).
- Shape: `[queue]` (required; `schema_version`, `name`, `mass_spectrometer`, `extract_device`, `tray`, `load`, `username`, `email`, `queue_conditionals`, `repository`, `[queue.delays]` `before_analyses`/`between_analyses`/`after_blank`/`extract_delay`), then `[[runs]]` with `identifier`, `aliquot`, `step`, `extraction` table, `measurement` table (`plan`, `hook`, `advanced`, `overrides`), `post_equilibration`, `post_measurement`, `overlap`, `overlap_min`, `delay_after`, `conditionals`, `truncate`, `comment`, `weight`, `skip`, `end_after`, `sample` table. Unknown keys are errors.
- Legacy short names accepted as aliases (`e_value`, `extract_units`, `s_opt`, `t_o`, … `queue_toml.cpp:17`–`28`); writer emits canonical names only.
- Round trip `parse_queue(dump_queue(q)) == q`; run order kept. `#` comments lost at first save (example queue's header comment included).
- Blocks: own files, `[block]` + `[[runs]]` (`libs/experiment/src/factory/blocks.cpp:14`).
- Disk: queue sits in lab directory beside plans and scripts (`elctl` `--lab` defaults to queue's directory, `apps/elctl/src/exp.hpp:10`). UI open/save `*.toml`, remembers one path (`experiment_window/last_queue`). No recent list, no autosave, no `rem`/`ex` copies.
- Tied to setup: template `profiles/instrument-common/experiment.toml` (rendered by `profile.toml:71`–`72`), `seed.toml:8`–`9`, checked by `tests/setup/test_profiles.cpp:81`–`83`, `:127`. Examples `configs/examples/experiment*.toml`, `blocks/blank_pair.toml`.
- No importer of legacy `.txt` queues anywhere (`libs/ingest`, `elctl`).

## 3. Comparison

| | legacy tab text | `experiment.toml` |
|---|---|---|
| Read by eye, one run a line | yes: a table | no: 4–8 lines a run |
| Edit in spreadsheet | yes (tab) | no |
| Diff in git | one line a run changes | stable canonical dump; more lines |
| Nested values (extraction, overrides, conditionals with kind, sample) | flattened columns, strings | native tables |
| Types | all strings; parser guesses | typed; bad value refused with line |
| Unknown key | ignored silently | error |
| Version | none | `schema_version = 1` |
| Comments kept on save | `##` lines dropped on save too | dropped |
| Header metadata | YAML, loose | `[queue]`, strict |
| Tooling cost now | none written | parser, writer, validation, setup template, tests all exist |

Neither keeps comments. TOML's cost is length per run, not expressiveness; legacy's is untyped flat strings and silent unknown keys.

## 4. Options for brainstorm (author's suggestion first)

A. Keep TOML as stored format. Add: (1) queue subdirectory (owner's minimum), (2) importer for legacy `.txt` queues (`elctl exp import-queue`, and UI Open accepts `*.txt`), (3) optional tab export for spreadsheet editing. Cheapest; no migration of existing queues.
B. TOML, one run per line: writer emits each run as inline table (`runs = [ {identifier = "66001", extraction = {...}}, ... ]`). Same parser (toml++ reads both forms), table-like diffs. Lines get long with nested values.
C. Tab table with TOML header (legacy-like, typed columns, `schema_version` in header). Spreadsheet-friendly; nested fields need column naming (`extraction.value`) or JSON-in-cell. Second parser and writer, migration of `.toml` queues and setup template.

Author leans A, with B as cheap follow-up if TOML length bothers operators.

## 5. Subdirectory (the minimum asked)

Legacy: `experiments/` under lab root, with `auto_save/`, `blocks/`, `rem/`. Here queue sits in lab directory, which also holds plans, scripts, conditionals; `elctl --lab` default reads queue's directory as lab, so moving queues to `<lab>/queues/` changes that default (would become parent of `queues/`) and `profile.toml`'s output path and `test_profiles.cpp`. Can be split off as own S row once directory name agreed.

## 6. Questions for owner

1. Format: A, B or C (section 4)?
2. Subdirectory name: `queues/` or legacy's `experiments/`? Blocks under it (`queues/blocks/`) or where they are?
3. Legacy `.txt` queue import wanted? Labs moving over have queues in that form.
4. Autosave and `rem`/`ex` copies (legacy `auto_save/`, `rem/`): wanted?
5. Comments kept on save: wanted (needs writer that preserves them)?

## Out of scope

Changing any code; measurement-plan or block formats beyond where blocks live; T024 (split queue tables in the editor).
