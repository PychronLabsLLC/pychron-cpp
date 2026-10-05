# Publication data export

How to get a 40Ar/39Ar data report that follows Schaen et al. (2021),
"Interpreting and reporting 40Ar/39Ar geochronologic data" (GSA Bulletin,
v. 133, p. 461-487, doi:10.1130/B35560.1), out of pychron in one step, from
`pychron-ui` or `elctl export`. The code is `libs/processing`
(`report.hpp`); the standard's own text is the authority on what a paper
must carry, this page says what the export puts where.

## One click

- **Data browser** (View > Data): filter or select analyses and press
  **Export**. A file dialog asks where; the name ends in `.csv` or `.json`
  and decides the format. The selected rows are exported, or every row shown
  when nothing is selected. The analyses are reduced on the processing
  thread and the status line says what was written.
- **Figure window** (an ideogram, spectrum or isochron): **Export table...**
  on the toolbar writes the figure's analyses with the figure's grouping and
  exclusions. From a spectrum the plateau criterion, weighting and error of
  the figure's options are used, so the table matches the plot.
- **Command line**:

  ```bash
  elctl export --db sqlite:/path/to/store.db --out FC-2-report.csv --sample FC-2
  elctl export --db postgresql://user:pw@host/db --out run.json --irradiation NM-300 --sigma 2
  ```

  `elctl export help` lists every flag. Selection flags (`--sample`,
  `--identifier`, `--project`, `--irradiation`, `--type`, `--uuid`, `--from`,
  `--to`) may repeat and every one narrows; without `--type` the unknowns
  are exported, and analyses tagged `invalid` are left out unless
  `--include-invalid`. Exit code 1 means nothing matched.

## Grouping

The summary has one row per group. The browser and `elctl export` group by
aliquot when any analysis is a heating step and by identifier otherwise
(`--group-by` overrides); a figure window exports its own grouping. The
exclusions of a figure (and the `omit` and `outlier` tags elsewhere) keep an
analysis in the table, marked `included = no`, and out of the plateau,
weighted mean and isochron. The integrated age uses every step unless the
spectrum's "Integrated age uses excluded steps" is off.

## What the file holds

A CSV report is one file in sections; a JSON report has the same content with
the metadata as an object and every other table as a list of row objects.

| Section | Rows | Contents |
|---|---|---|
| `metadata` | item, value | the standard, generator and date, the reduction version and constants preset, laboratory, counts, instruments, analysts, projects, samples, materials, irradiations, fluence monitors, the uncertainty level, age and intensity units, what the isotope columns are corrected for, what each age uncertainty contains, and the plateau, mean and isochron definitions |
| `constants` | one per constant | lambda_e, lambda_beta, lambda_K total, lambda_37Ar, lambda_39Ar, lambda_36Cl, atmospheric 40Ar/36Ar and 40Ar/38Ar, abundance sensitivity, a fixed (37Ar/39Ar)K or cosmogenic ratios when used, whether the decay-constant uncertainty entered the ages |
| `irradiation` | one per identifier | sample, material, project, PI, latitude, longitude, elevation, lithology, unit, location, IGSN, irradiation, level, position, reactor, irradiation start and total duration, fluence monitor with material and age, J, J position error, a lambda_K override, and the production ratios (40Ar/39Ar)K, (38Ar/39Ar)K, (37Ar/39Ar)K, (39Ar/37Ar)Ca, (38Ar/37Ar)Ca, (36Ar/37Ar)Ca, (36Ar/38Ar)Cl, Ca/K, Cl/K |
| `analyses` | one per analysis | run id, identifiers, group, aliquot, step, time, extraction (value, units, duration, cleanup, weight), included, tag; 40Ar to 36Ar corrected intensities and the blank subtracted from each; 40Ar*/39ArK, %40Ar*, 39ArK (fA, % of the group, cumulative %), K/Ca, K/Cl; the inverse-isochron coordinates 39Ar/40Ar and 36Ar/40Ar with their correlation; the age with its analytical and with-J uncertainty; a note when the analysis has no age and why |
| `summary` | one per group | sample, identifier, material, counts, whether step heated; integrated age; plateau steps, n, %39ArK, age, MSWD, p; weighted mean n, age, MSWD, p; isochron n, age, trapped 40Ar/36Ar, MSWD, p; a note for groups without ages |

Every `±` column is at the level the metadata states, two sigma by default
(`--sigma 1` for one). Ages carry two uncertainties: *analytical* (the
measured intensities, baselines, blanks and detector factors) and *with J*
(the J uncertainty of the position added). The decay-constant uncertainty is
included when the reduction was run with it (`--decay-error`); the fluence
monitor's age uncertainty is never propagated, and the metadata says so.
Isotope intensities are corrected for baseline, blank, detector
intercalibration and discrimination and, for 37Ar and 39Ar, for decay since
irradiation; interfering-reaction corrections are applied in 40Ar*/39ArK,
39ArK, K/Ca and K/Cl, not in the intensity columns.

Numbers are written to 12 significant digits, never rounded for display; a
cell the source has no value for is empty (`null` in JSON). Text with
commas or quotes is quoted as RFC 4180 says, so a spreadsheet opens the CSV
directly.

## Where the metadata comes from

The sample's location, lithology and IGSN are the catalog's sample row
(Entry > Samples, or `elctl entry samples import`); the fluence monitor, its
material and age are the position's flux record; the reactor is the level's
production record. What the store does not have is left empty rather than
guessed, so a report from a freshly imported legacy database may have empty
location columns until the samples are filled in.
