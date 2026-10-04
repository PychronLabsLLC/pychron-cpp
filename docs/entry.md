# Sample and package entry

How to enter samples and packages (irradiations) in the DVC store, from
`pychron-ui` or `elctl entry`. The design is
`superpowers/specs/2026-10-04-sample-irradiation-entry-design.md`.

## Before you start

- Entry writes the store only: the shared PostgreSQL database of the lab (or
  a SQLite file for trying things out). It never writes the legacy MySQL
  database or the meta repository.
- A lab that still has legacy pychron data migrates it first
  (`elctl import`, see `legacy_import.md`). After that the store is the
  catalog, and legacy Python acquisition is not supported.
- Entry needs the server. Nothing here works from an offline export.

## Words

| Word | Meaning |
|---|---|
| package | A set of levels of positions that hold samples. The generic idea of an irradiation. |
| package kind | `irradiation` (a chronology, interference productions and flux) or `package` (positions and samples only). Each package has its own; it can be changed later without losing anything. |
| level | A tray of a package (A, B, ...), on an irradiation holder. |
| position | A hole of a level's holder, numbered from 1. |
| identifier | The labnumber of a position, given out in sequence. |

## Samples

Entry > Samples… lists the samples (filter by PI, project, material, or
search by name).

- Double-click a cell to edit it. Edited cells are tinted until you Save.
  Save writes every edit at once.
- If another person changed one of those samples since you loaded it, nothing
  is saved. The row turns yellow, and its tooltip shows their values.
  Reload, then edit again.
- New sample: fill in the form above the table, then Add, then Save.
  - The PI is "Last" or "Last, F". Names that do not fit that pattern, such as
    a lab name, can be allowed in Entry Settings.
  - A PI, project or material that does not exist yet is created with the
    sample.
  - A warning appears when a similarly named sample exists in any project
    ("FC-2", "fc 2" and "FC_2" all count as the same name).
- Delete is refused for samples that sit in a position or have analyses.

Entry > Import Samples… (or Import… in the Samples window) reads a CSV file,
or rows pasted from a spreadsheet.

1. Write Template… saves a CSV that has every column.
2. Columns are matched by name, and common aliases work (`pi`, `latitude`,
   `grain size`, ...). Change the match in the mapping table if needed.
3. The preview gives each row a state:

   | State | Meaning |
   |---|---|
   | create | The sample is new. |
   | exists | The sample is already stored with the same values. |
   | update | The sample is stored with different values. It is written only when "Update samples that differ" is ticked. |
   | error | The row has a problem; every problem is listed. |

4. While any row is in error, nothing can be imported. Export Errors… saves
   those rows to fix them.

UTM coordinates (easting, northing, zone such as `13S`) are converted to
latitude and longitude. A zone letter before N means the southern hemisphere.

## Packages

Entry > Packages… shows packages and their levels on the left, and the
positions of the chosen level in the middle. The dock on the right has four
tabs: Samples, Level, Chronology and Holder.

**New Package…**

- Choose the kind. The name is the next one with the lab's prefix
  (NM-301 after NM-300).
- Give the levels as `A-C` or `A,B,D`, with a holder and z for them.
- An irradiation also needs a reactor (its production is copied from the
  reactor defaults) and its doses (power, start and end, in local time).
- Everything is written in one step.

**New Level…** pre-fills the next letter, and the holder, z and production of
the level you are looking at.

**Positions**

- Select rows, or click and drag on the Holder drawing.
- Pick a sample in the Samples tab, then press Assign to selected.
- Weight, packet and note are edited in the grid.
- Clear Fields… blanks the chosen fields of the selected positions.
- Fill Packets… numbers packets from the one you give: P7, P8, ...
- Save writes the level: positions, note, holder, z and production, in one
  step.
- Moving to another level with unsaved edits asks whether to save them.
- Clearing a sample never removes a position's identifier.
- Changing the sample of a position that has analyses asks first, and says
  how many analyses it changes.

**Identifiers**

- Generate Identifiers… numbers every position that has a sample and no
  identifier. Numbering continues the store's sequence (the highest numeric
  identifier, then +1, +2, ...), level by level in name order, position by
  position.
- The preview is exactly what is written. If someone else gave out
  identifiers in the meantime, the dialog shows the new plan instead of
  writing.
- "Renumber identifiers nothing has used yet" gives new numbers to positions
  whose identifiers have no analyses. Numbers are never reused.
- For an irradiation, the dialog warns about a level with no monitor, and
  about a monitor outside project `Irradiation-<package>`.

**Other tools**

- Edit Production… on the Level tab edits a production's interference ratios,
  creates a new production, or copies a reactor default.
- The Chronology tab edits the doses of an irradiation.
- Import Positions… / Export CSV… use the columns `level, position,
  identifier, sample, project, principal_investigator, material, grainsize,
  weight, packet, note, j, j_err`. Identifier and J are not read back.
- Save PDF… writes the level sheets: a summary, then each level with its
  holder and a row for every hole.

Entry > Holders… lists irradiation holders and imports legacy holder files
(`circle,<radius>[,True]`, then one hole per line).

Entry > Entry Settings… holds the lab's settings, which every client shares:

- the package name prefix and the kind new packages start as
- PI names allowed besides "Last, F"
- the monitor sample and material
- the estimated J per hour of dose
- whether positions without an identifier need a packet

## elctl entry

The same work from a shell. Writing commands take `--db <url>`, `--user`
and `--dry-run`. `elctl entry help` lists every option.

```
elctl entry samples template samples.csv
elctl entry samples import samples.csv --db postgresql://... [--update-existing] [--errors bad.csv]
elctl entry holders import 24-hole.txt --db ...
elctl entry package add NM-301 --levels A-C --holder 24-hole --z 0.5 \
      --reactor Triga --chronology chron.txt --tz America/Denver --db ...
elctl entry package add P-7 --kind package --levels A --db ...
elctl entry positions import NM-301 positions.csv --db ...
elctl entry identifiers generate NM-301 --dry-run --db ...
elctl entry identifiers generate NM-301 --db ...
elctl entry package show NM-301 --csv --db ...
elctl entry settings set pi_names_allowed '["NMGRL Lab"]' --db ...
```

Exit codes:

| Code | Meaning |
|---|---|
| 0 | It worked. |
| 1 | Nothing was written: a row was stale, refused or invalid. The output says which. |
| 2 | A usage error or a fatal error. |
