# Importing legacy pychron data

`elctl import` brings data from a legacy pychron installation into a new
store (SQLite or PostgreSQL) with its full history. It can be stopped and
started again, it can be run again on the same data without duplicating
anything, and it can check its own work. This page is for the person who runs
the import; developers should also read
`docs/superpowers/specs/2026-10-03-legacy-ingestion-design.md`, whose section
10 holds the rulings behind what is written here.

## 1. What can be imported

There are three kinds of source, registered with `--kind`:

| Kind | What it is | Where it comes from |
|---|---|---|
| `legacy_db` | The catalog: samples, projects, irradiations, identifiers, users, tags. | A mysqldump of the legacy database, converted with `tools/legacy_dump_to_jsonl.py` |
| `meta_repo` | The MetaData repository: irradiations, levels, productions, chronologies, flux, holders. | A git repository or its url |
| `project_repo` | A DVC data repository: the analyses of one project, or of one spectrometer's blanks, airs and cocktails, with every later edit in its git history. | A git repository or its url |

A local path is read in place and never modified. A url is mirrored into the
cache directory (a `git clone --mirror`) and fetched again on every `run`.

### Converting the database dump

The importer does not talk to MySQL. Convert the dump once:

```bash
python3 tools/legacy_dump_to_jsonl.py pychrondvc.sql catalog/
```

`catalog/` then holds one `<Table>.jsonl` per table and a `MANIFEST.json`
(written last: a directory without it is not a complete conversion). The
directory is what `--source` takes for a `legacy_db`. If you have no dump you
can still import project repositories with `--catalog-from-repos` (below).

## 2. The recommended order

1. **Catalog** (`legacy_db`), if you have a dump. It gives the importer the
   identifiers, positions and tags that analyses refer to. An analysis whose
   identifier the catalog does not have is refused (`unknown_analysis`)
   until it does.
2. **MetaData** (`meta_repo`). Reduction needs its flux, productions and
   chronology.
3. **Repositories of reference runs** (blanks, airs, cocktails; the
   per-spectrometer repositories), registered with `--reference-runs`.
4. **Project repositories**.

You do not have to register the sources in this order: `run --all` sorts
them (catalog, meta, reference-run repositories, other project repositories,
each by url). What matters is that the reference-run repositories are marked.
A blank or IC-factor reference inside a project's analysis points at an
analysis in another repository. If that analysis is not in the store yet, the
reference is kept unlinked, and it stays unlinked even after the other
repository is imported later. Importing the reference-run repositories first
is what links them.

Use one `--cache` directory for the whole import. It holds the settings of
each source (`<source id>.toml`: time zone, branch, flags) and the mirrors of
remote repositories. Those files are part of the import's state: keep them
with the database, because `run`, `verify`, `status` and `conflicts` need
them. The default is `~/Library/Caches/pychron/import` on macOS,
`$XDG_CACHE_HOME/pychron/import` (or `~/.cache/pychron/import`) elsewhere,
`%LOCALAPPDATA%\pychron\import` on Windows.

## 3. The commands

Every command takes `--db <url>`: `sqlite:/path/to/store.db` or
`postgresql://user@host/db`. `elctl import help` lists everything. Only `add`
and a real `run` create or migrate a database; the other commands need one
that exists.

### add

Registers a source and prints its id. Registering the same source again
changes nothing, and nothing is stored if any check fails.

```bash
elctl import add --db sqlite:store.db --kind legacy_db --source catalog/ --tz America/Denver
elctl import add --db sqlite:store.db --kind meta_repo --source https://github.com/NMGRLData/MetaData --tz America/Denver
elctl import add --db sqlite:store.db --kind project_repo --source ~/data/Felix_blank180 --tz America/Denver --reference-runs
elctl import add --db sqlite:store.db --kind project_repo --source ~/data/IR1010 --tz America/Denver --catalog-from-repos
```

- `--tz` is the lab's IANA time zone. Legacy files hold naive local times, so
  the zone decides every timestamp the import stores. It cannot be changed
  once a source is registered: `add` with another zone is refused.
- `--branch` names the branch to read (default: the one HEAD names).
- `--author-map file.toml` maps git author emails to user names, one line
  each: `"ann@example.org" = "Ann Author"`.
- `--catalog-from-repos` (project repositories, for use without a dump) makes
  identifiers, positions and spectrometers from the analysis records. Each
  identifier made up this way leaves one warning conflict (`synthesized`).
- `--reference-runs` marks a repository of blanks, airs and cocktails.

### run

```bash
elctl import run --db sqlite:store.db --all
elctl import run --db sqlite:store.db --source IR1010 --limit 3
elctl import run --db sqlite:store.db --source IR1010 --dry-run
elctl import run --db sqlite:store.db --source IR1010 --replay
```

A progress line per batch goes to stderr; at the end of each source a summary
goes to stdout with what was written and the conflicts pending.

- **Resuming.** `run` continues from where the last run stopped. Ctrl-C
  finishes the batch being written and stops with the message
  `paused: <name> at <done>/<total>`; run the command again to go on. A
  second Ctrl-C ends the program at once. `--limit N` does the same after N
  batches (`--batch` sets the batch size, default 500 commits).
- **`--replay`.** Walks the source again from the start. What is already
  imported is left alone; analyses that were refused earlier are imported
  now. Use it after you have fixed the catalog (for example after importing
  the dump you had left out): the summary tells you so when
  `unknown_analysis` conflicts are pending. The conflicts it resolves become
  `superseded`.
- **`--dry-run`.** Writes nothing and fetches nothing; prints how many rows a
  run would write. It needs an existing database and, for a url source, a
  mirror already in the cache.
- Exit code: 0 when finished or paused, 1 when a source finished with
  blocking conflicts pending, 2 on an error.

### status

```bash
elctl import status --db sqlite:store.db
```

One line per source, in run order: id, kind, name, status (`registered`,
`running`, `paused`, `finished`, `failed`), commits done/total, and the commit
the import is at.

### conflicts

```bash
elctl import conflicts --db sqlite:store.db
elctl import conflicts --db sqlite:store.db --source IR1010 --kind unknown_analysis --json
```

A conflict is a file or row the import could not take, or took with a note.
It never stops an import. By default the pending ones are listed, one line
each: kind, path, entity, detail. `--all` adds resolved and superseded ones.

Conflicts are **blocking** when data was not imported or does not agree, and
**warnings** when they only annotate something that was imported. Verify
fails on blocking ones only.

| Kind | Meaning |
|---|---|
| `unparseable` | A file could not be read (the reason is in the detail: invalid JSON, a chronology line that cannot be read, a spectrometer file that appeared after the analysis it belongs to). Blocking. |
| `unknown_analysis` | An analysis, or a later file of it, was refused because the catalog has no such identifier. Blocking until `run --replay` imports it. |
| `identity_clash` | Two things claim the same identity (a run id or position already holding another analysis). Blocking when something was refused. A warning when the row was imported without a broken optional link (`imported`), when the catalog row was made from the repositories (`synthesized`), or when an older version could not be placed behind a newer one (`late_revision_not_applied`). |
| `value_mismatch` | The age verify recomputed does not match the legacy age (the detail has both values and what was used). Blocking. |
| `hand_edit` | Reserved: a published repository changed by hand. Not written yet. |
| `provisional_renumber` | Reserved; the importer does not write it. |

### verify

```bash
elctl import verify --db sqlite:store.db
elctl import verify --db sqlite:store.db --source IR1010 --tolerance 1e-7 --constants legacy_preferences --json
```

For each source, `verify` checks:

1. **The import is finished** and the source has not moved since. A url source
   is not fetched by `verify`; it reads the mirror as the last run left it.
2. **Accounting.** Every file at every commit is imported (has a provenance
   row), is listed as a conflict, or is a repeat of content the walk already
   imported. Anything else is listed as unaccounted.
3. **Idempotence.** A second run, resumed or replayed, would write nothing.
4. **Age parity.** For each interpreted age in the repository, every analysis
   it lists is reduced again from the imported data, as the data stood when
   the interpreted age was saved, and the age and its error are compared
   with the legacy ones. Each is reported as `pass`, `pass on age only` (the
   file does not say which kind of error it holds), `fail` or `not
   comparable`, with the reason for the last (for example an analysis in
   another repository, reference data changed since, no J). A failure is also
   stored as a `value_mismatch` conflict. The output ends with the largest
   residual among the passes, so a drift that is still inside the tolerance
   stays visible.
5. **No blocking conflict is pending.**

`ok` means 1 to 3 and 5 hold and no parity comparison failed. Members that are
not comparable do not make a source not ok, so read the `not comparable`
lines: a source whose parity is all "not comparable" has not had its ages
checked.

Options:

- `--constants` (default `legacy_preferences`; also `legacy` and `default`)
  selects the decay constants and atmospheric ratios the ages are computed
  with. `legacy_preferences` is what legacy pychron used unless a lab changed
  its preferences. The store keeps no constants per analysis; a lab whose
  preferences differed must say which preset is closest.
- `--tolerance` (default `1e-6`) is a relative difference, applied to the age
  and to its error.
- `--source` limits the check to one source; `--json` prints a report per
  source for scripts.

Exit code: 0 when every source checked is ok, 1 when one is not, 2 on an error
or when nothing is registered.

## 4. Credentials

A source url must not contain a user or a password: `elctl import add` refuses
`https://user:token@host/...` and `ssh://git@host/...` before it fetches
anything, and never prints the url it refused. Let a git credential helper
supply them (`git config --global credential.helper ...`), or use the scp-like
form `git@host:path` with an ssh agent. The database url may hold a password
for PostgreSQL; prefer a password file or the environment your PostgreSQL
client reads.

## 5. Known limits

These are things the importer does not do, or does differently from what you
might expect. Section 10 of the design spec has the full wording.

- **Edits made in the new application stay.** Once an item has been changed
  in the new application, the legacy source no longer updates it. A later
  edit to the same item in the legacy repository arrives as a warning
  (`late_revision_not_applied`) with its content kept in the conflict. The
  opposite holds for the first import of a source: it overwrites the heads
  that already exist for the items it brings.
- **Several workstations.** When a repository is merged from several
  workstations and imported in more than one run, the revision history can
  lack intermediate revisions that one uninterrupted import of the final
  history would hold. The current values (heads) agree.
- **Merge order and bookmarks.** Git's order can put commits of a branch that
  has not been merged yet before an unrelated merge. A bookmark made from a
  git tag on that merge can then include a value from the unmerged branch;
  heads are right once the branch is merged. A git tag added later to a
  commit already imported becomes a bookmark only when a replay sees it.
- **Analyses still being collected.** An analysis whose files are incomplete
  when an incremental run ends is imported as it stands (a synthetic
  collection); one longer uninterrupted import would have found it complete.
  The later files arrive as revisions.
- **Several analyses in one commit.** Two analyses that swap run ids in one
  commit both become `identity_clash`. A collection in the same commit as the
  renumbering that frees its run id is refused until a replay. A record
  without a uuid that is renumbered reads as a different analysis.
- **Refused, then renumbered.** An analysis refused in one run, then
  renumbered, whose old run id another analysis took, is an `identity_clash`
  on replay.
- **The catalog's existing rows win.** When a catalog row already exists in the
  store, verify cannot tell that the dump's other columns were not applied.
  For a catalog, `ok` means the dump was imported, not that every column of
  every row was.
- **Reference data that cannot say "removed".** When a position, a production
  or a flux entry is deleted from MetaData the importer writes a revision
  that says there is no value. A kind of reference data that cannot express
  absence keeps its old value as head; the removal is recorded only in the
  changeset's provenance note.
- **Ages can only be checked against the same repository's data.** Members
  imported from another source, and members whose reference data (flux,
  production, chronology) changed after the interpreted age was saved, are
  reported as not comparable, not as passes.
- **Run logs** (`logs/*.logs.log`) are not imported; they count as accounted
  for.
