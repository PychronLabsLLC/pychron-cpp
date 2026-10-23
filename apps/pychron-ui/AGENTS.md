# pychron-ui (`apps/pychron-ui`): agent rules

Part of the agent guide: root `AGENTS.md` applies in full, this adds what is
only this component's. Kept compressed, as that file is.

- File > Preferences keeps logging and the line's `[metrics]` in files, not
  QSettings: `elctl` must follow them. An install's logging is
  `<root>/logging.local.toml` (keys at top level, no `[logging]` table;
  `config::replace_local_logging`); Restore Defaults goes to `logging.toml`,
  built-in defaults without one. With no install root (examples, files given)
  logging is the line's `[logging]` in its local override file, as `[metrics]`
  always is. `config::replace_local_table` (`libs/core` `local_file.hpp`) is
  the only thing that writes the line's file after the setup wizard; it
  replaces one table and leaves the rest byte for byte. Page's "No log file"
  is `dir = ""`; unchecked and empty field is unset `dir` (default folder). A table read over another (local file's
  over main one's) must leave alone every key it does not name: a new key in
  `parse_logging` or `parse_metrics` gets a case in
  `ConfigOverride.Local...KeysWinAndTheRestStay`.
- Live plot (strip chart, evolutions, peak center, gauge history, holder,
  pattern canvas, dock label) takes series colours and background from
  `plot_style()` / `plot_series_color(slot)` (`plot_style.hpp`) and connects
  to `PlotStyleHub::instance()`'s `changed()` in its constructor to restyle
  an open view. Never a colour literal, never `theme()` for these: grid and
  axis stay theme's. A detector colour from spectrometer config carries no
  `DetectorSeries::palette_slot` and is never overwritten by a palette
  change. Figures (`libs/processing` scene) do not read `plot_style()`: their
  colours are their options' and presets'.
