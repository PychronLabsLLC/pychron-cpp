# Color spelling: "color", not "colour" (as built)

Row T085 (from T079, T081). Draft for owner's review. Small fix, written after the work: records rule and its edges, nothing left to implement.

## Rule

Text user reads says "color": UI labels, notes, dialog titles, figure-option labels, user guide (`docs/user`). Matches config keys (`color`, `[colors]`, `open_valve_color`) and Qt API, which always said "color".

## State

| Place | Text | Where landed |
|---|---|---|
| Preferences, `apps/pychron-ui/src/preferences_dialog.cpp` | `"No color"`, `"Row color"`, `"Reset colors"`, Data note ("Row colors of the data browser ...") | `286a7c01` on `origin/develop` |
| Figure options, `libs/processing/src/figure_common.cpp` | group row label `"Color"` (`optional_color("color", "Color", "Group")`) | `286a7c01` on `origin/develop` |
| Data browser | `"Color by"` | `322e9039` on `origin/develop` |
| Detector readout | swatch column `Color` | `6cb839a2` on `origin/develop` |
| User guide, `docs/user/02,03,04,06,07-*.md` | every "colour" in prose | `425af785` (rebased `fe19d4b9`) on `docs/color-spelling-guide`, not yet on `origin/develop` |

After branch lands: `git grep -i colour -- docs/user` empty; no user-visible string under `apps/` or `libs/processing` says "colour".

## Out of scope

- Code comments (`apps/pychron-ui/src/canvas_*.{hpp,cpp}` and others): read by developers, not users. Left; change when touching the function anyway, not in sweep.
- `libs/earthbank` messages about EarthBank template "colouring" (`xlsx.cpp`, `ooxml.cpp`): name template's own term. Left.
- Tests asserting a key named `colour` is refused (`apps/elctl/tests/test_earthbank_commands.cpp`, `test_export_cmd.cpp`): "colour" there is the wrong input under test. Must stay.
- Config keys: already `color`; no rename, no alias, no migration.
- Translations: none shipped.

## Remaining

Close T085 once `docs/color-spelling-guide` lands on `origin/develop`. No code change, no test.
