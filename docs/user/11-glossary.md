# Glossary

Terms as pychron uses them. Longer explanations are linked.

| Term | Meaning |
|---|---|
| **Aliquot** | One heating step or one portion of a sample; steps of one sample share an identifier and differ by aliquot. See [data analysis](06-data-analysis.md). |
| **Analysis** | The saved result of one run: intensities, baselines, metadata, and tags. |
| **Age spectrum** | Figure of apparent age against cumulative 39Ar released, per heating step. |
| **Air / blank / cocktail / unknown** | Run types. Unknowns are samples; air and cocktail are standards; blanks are gas-free extractions. See [experiments](04-experiments.md). |
| **Block** | Reusable group of runs in a queue (for example a blank pair). |
| **Canvas** | The drawn diagram of the extraction line in `pychron-ui`. See [extraction line](02-extraction-line.md). |
| **Conditional** | A rule checked during a run (terminate, truncate, cancel, ...). See [experiments](04-experiments.md). |
| **Detector IC** | Intercalibration factor between detectors. |
| **Doctor** | `elctl doctor`, the config checker. |
| **DVC store** | The shared database (PostgreSQL, or SQLite for trials) that holds analyses and the sample catalog. |
| **elctl** | The command-line tool. See [elctl reference](08-elctl-reference.md). |
| **Extraction line** | Valves, pumps, gauges and heaters between sample and spectrometer. |
| **Extraction script** | A Python script that drives one run's extraction. See [scripting](09-scripting.md). |
| **Hop** | A measurement step that moves the magnet so detectors see chosen isotopes. See [spectrometer](03-spectrometer.md). |
| **Identifier** | The labnumber of a position, given out in sequence. See [entry](../entry.md). |
| **Ideogram** | Probability-density figure of ages. |
| **Install / profile** | An install is one configured computer set-up; a profile is the template (data-reduction, Argus, Helix, NGX). See [configuration](07-configuration-reference.md). |
| **Interlock** | A valve rule that forbids opening a valve while another is open. |
| **Inverse isochron** | 36Ar/40Ar against 39Ar/40Ar figure. |
| **Level / position** | A tray and a hole of an irradiation package. See [entry](../entry.md). |
| **Measurement plan** | TOML file describing what the spectrometer does during a measurement (multicollect, hops). |
| **Package** | Set of levels and positions holding samples; kind `irradiation` or `package`. |
| **Peak center** | Finding the magnet position that centers a peak on a detector. |
| **Preset** | Saved named set of figure options. |
| **Queue** | A list of runs to execute, in `experiment.toml`. |
| **Recall** | Re-opening a saved analysis. |
| **Run** | One extraction plus measurement. |
| **Spool** | Local holding area for records that could not yet be saved to the store. |
| **Simulation (`--sim`)** | Everything runs against built-in simulated hardware. See [The simulated lab](../simulator.md). |
| **Strip chart** | Live intensity-versus-time plot in the spectrometer window. |
| **Tray map** | File listing the holes of a sample tray and their coordinates. |
| **Truncate** | Cut a run's measurement short based on a conditional. |
