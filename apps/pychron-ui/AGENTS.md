# pychron-ui (`apps/pychron-ui`): agent rules

Part of the agent guide: root `AGENTS.md` applies in full, this adds what is
only this component's. Kept compressed, as that file is.

- File > Preferences keeps the line's `[logging]` and `[metrics]` in the
  line's local override file, not in QSettings: `elctl` must follow them.
  `config::replace_local_table` (`libs/core` `local_file.hpp`) is the only
  thing that writes that file after the setup wizard; it replaces one table
  and leaves the rest byte for byte. A table read over another (local file's
  over main one's) must leave alone every key it does not name: a new key in
  `parse_logging` or `parse_metrics` gets a case in
  `ConfigOverride.Local...KeysWinAndTheRestStay`.
