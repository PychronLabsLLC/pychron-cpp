# Finished runs to the store (`libs/experiment`): agent rules

Part of the agent guide: root `AGENTS.md` applies in full, this adds what is
only this component's. Kept compressed, as that file is.

- Finished runs reach the store through `libs/experiment/adapters/store`
  (`pychron::experiment_store`, namespace `pychron::experiment::store`),
  built only with persistence, like `libs/ingest`; `libs/experiment` itself
  never links `libs/persistence` and sees only `IAnalysisPersister`. Tests:
  `tests/experiment/` (`pychron_experiment_store_tests`). Spec:
  `docs/superpowers/specs/2026-10-10-e2e-sim-experiment-design.md`. Rules
  easy to break:
  - `to_ingest` (`record_ingest.hpp`) is pure: no clock, no random id, no
    store read. Every id is `Uuid::v5(<record uuid>, label)`, labels
    `changeset`, `signals`, `intercepts`, `baselines`, `blanks`,
    `icfactors`, `tags`, `item`, `blob:<sha hex>`. They are a file format:
    a changed recipe makes a re-sent record a second analysis.
  - It refuses (`Config` error, record stays in spool) what store cannot
    hold: record not finalized, uuid or timestamp that does not parse, step
    that is not letters, series of mismatched lengths, two series of one
    kind for one isotope on one detector, intercept whose isotope has no
    series. A non-finite number is stored as unknown, not refused; text
    that is not UTF-8 has its bad bytes replaced, never thrown. Field added
    to `AnalysisRecord` gets a row in
    `tests/experiment/test_record_ingest.cpp` or a place in spec's
    not-stored list.
  - Names: `libs/experiment` `record/names.hpp` is the one place that says
    what an analysis calls a record's series and intercepts. Records path
    (`analysis_from_record`, `raw_from_record`), record builder and
    `to_ingest` all go through it. Record keeps acquisition's names
    (`Ar40:H1` for an isotope on several detectors); an analysis and the
    store have `H1:Ar40`. Isotope on one detector keeps its plain name. One
    detector's baseline series are one stored series, in record's order
    (the order they were fitted in). An ordinary run's stored items must
    never change: `RecordIngest.AnOrdinaryRunConvertsAsItAlwaysDid` pins
    them and is not edited to make a change pass.
  - Every plan an install ships must give a record the store takes and that
    reads from the store as the record reads
    (`InstallE2E.EveryShippedPlanIsStoredAndReadsAsItsRecord` runs each). A
    new plan shape (another series kind, another way to share a detector)
    is run there before it ships. Only `argus` is installed by that test.
  - A run whose record the store did not take is never a plain success:
    `SavePipeline::save` answers `Saved::pending`, run is flagged
    `save_error` and notes `run::kNotSavedNote` plus the reason. Whoever
    reports runs goes by `save_error`, not by state alone.
  - Persister never makes a catalog row (identifier, sample, project, mass
    spectrometer, extract device); `check_queue` refuses, at its start, a
    queue the catalog cannot take. It is asked about what will run
    (`persist::rows_from`: rows before the start row, and rows an edit can
    no longer change, are marked skipped), at `LabSession::start` and at
    `LabSession::edit`. Whatever `to_ingest` would refuse for a reason known
    before the run (a step that is not letters) is refused there too.
  - A record leaves the spool only after the store has acknowledged it, and
    the store is written before `RunFinished` is published.
  - Spool flushed off control path (executor hardening spec 4.8):
    `persist::SpoolFlusher`, one-shot scheduler jobs, never a thread of its
    own, never on executor's thread. Flush at queue start and after every
    run (`ExecutorContext::flusher`); while pending > 0 every 5 min, wait
    doubling to 30 min while flushes fail, back to 5 min after one that
    succeeds; nothing scheduled with nothing pending. `SpoolStatus` published
    on change only. No run waits on it.
  - Aliquots, constraint on a later store-backed allocator (spec 4.8; not
    built): it answers offline from last known highest aliquot per
    identifier, and reconciles with store on flush. A conflict found there is
    flagged to operator; a record is never renumbered silently.
  - A store handle works only on the thread that opened it. Programs use
    `store::Acquisition` (`open_acquisition`, called from main thread),
    which owns that thread. It is declared before the session that saves
    through it: no call may be in progress or made once it is being
    destroyed.
  - One acquisition program at a time per database, as for the importer.
  - A database that is named (`--db`, or an install's) and cannot be used
    never falls back to record files.
  - Simulated runs. Which session is simulated is one function,
    `spectrometer::simulated_session` (`libs/systems` `bringup.hpp`); both
    programs give its answer to `SessionHardware::simulation`, and the
    record carries it (`instrument.simulated`, optional key written only
    when true: record version stays 2 and old records keep their sha;
    `tests/data/records/v2.*` hold that). Where such a session saves is one
    function too, `store::saves_simulated_runs`: the install's own
    `sqlite:` database when not given with `--db`, or anywhere with
    `--sim-save`; otherwise record files and one line saying so. Neither
    rule is restated in a program.
  - How run ended: `provenance.status` (`complete`, `truncated`, `failed`)
    and `provenance.failure`, each written only when not default (not
    `complete`, not empty), so complete record keeps bytes and sha; absent:
    `complete`. Version stays 2. `failed` record stored as any run, tag
    `invalid`, failure text its note (`RecordIngest`); records path reads
    it `invalid` too (`analysis_from_record`). `truncated` stored as any run,
    tag `ok`. Spec: `docs/superpowers/specs/2026-10-09-executor-hardening-design.md`
    4.5.
  - A database url is shown only through `store::without_password`; an
    error text of an `Acquisition` already has url and password taken out.
    The acquisition worker's `try` is the library's one catch (thread
    boundary).
