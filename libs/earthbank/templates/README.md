# EarthBank workbook templates

The two workbooks here are the vendor's import templates for EarthBank
(AuScope's AusGeochem platform):

| File | SHA-256 |
|---|---|
| `ArArDataPoint_Template2026.xlsx` | `85a5b59f04beff5e483eb787d4546916992754e9bd69071885ba10df16fa3997` |
| `Sample_Template_v2025-04-16.xlsx` | `1a2f516aca203cf342020272a4f4a5b8fde14a0a13a3e544a35a7c5c54d69f25` |

They were supplied by AusGeochem / EarthBank and reached this repository
through the University of Melbourne fork of pychron (branch `dev/dc2025`,
`pychron/ausgeochem/templates/`).

**They are the vendor's files and must not be edited**, opened and saved in a
spreadsheet program, or regenerated. The exporter copies a template and fills
in data rows; everything else in the copy is the template's, byte for byte,
because the website's importer has only ever been shown these files. A newer
template from the vendor replaces a file here whole (and its hash above), as
long as the field-name row still names the fields.

Permission to redistribute the templates in pychron's installers has not been
confirmed with AuScope. It is an open question for the owner
(`docs/superpowers/specs/2026-10-07-earthbank-design.md`, section 11) and must
be settled before the first release that ships them.
