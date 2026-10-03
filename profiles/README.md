# Setup profiles

What `elctl init` and the setup wizard install (installation wizard spec,
`docs/superpowers/specs/2026-10-03-installation-wizard-design.md`). Each
directory is a profile: `profile.toml` (questions and files) plus the
templates it renders. `@examples/` in a `copy` names `configs/examples`;
`convert = "legacy_line"` or `"legacy_canvas"` with a `copy` naming a folder
converts a legacy Pychron setup (`setup::import_legacy_line`) instead of
copying. Question types: string, host, port, int, float, bool, choice, path
(a file), folder, secret, list, table.

| Profile | Kind | What it sets up |
|---|---|---|
| `data-reduction` | data_reduction | a data folder and a local or server database |
| `argus` | instrument | Thermo Argus VI over Qtegra |
| `helix` | instrument | Thermo Helix over Qtegra |
| `ngx` | instrument | Isotopx NGX |
| `instrument-common`, `lab-common`, `extraction-line-starter` | fragment | shared pieces |

Templates use `{{ name }}`, `{{ name | toml }}`, `{% if %}`/`{% else %}`/
`{% endif %}` and `{% for x in list %}`/`{% endfor %}` (see
`libs/setup/include/pychron/setup/template.hpp`). Every profile is tested:
it renders with its defaults, the result loads, and doctor passes.
