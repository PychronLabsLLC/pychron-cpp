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

The importer reads these tables: `PrincipalInvestigatorTbl`, `ProjectTbl`,
`MaterialTbl`, `SampleTbl`, `IrradiationTbl`, `LevelTbl`,
`IrradiationPositionTbl`, `UserTbl`, `MassSpectrometerTbl`,
`ExtractDeviceTbl`, `LoadTbl`, `LoadPositionTbl`, and, for the tags of
analyses that have no tags file, `AnalysisTbl` and `AnalysisChangeTbl`. Every
other table of the dump is converted too and stays in the JSON-lines
directory, unread: keep the directory.

Legacy pychron writes `---------` (nine hyphens) where a value is absent. In
an analysis record and its extraction file, and in the text columns of the
dump that hold an optional link or free text, that string, any other run of
hyphens (`---`), or one that is empty or only white space, means "not set": an extract device, load, tray,
pattern, sample, material, project, irradiation, analyst, comment or note so
written is absent, not a name to look up. The analysis keeps the original
string in its legacy JSON. It is taken as written for an analysis's
identifier, uuid and mass spectrometer, and for the names that are a catalog
row's key (a material called `---------` stays a material).

The legacy database allows a sample without a material; the store does not.
A `SampleTbl` row whose `materialID` is empty, or names a material that is
not in the dump or was refused, is imported under a placeholder material
named `unknown` (no grainsize), and its irradiation positions keep the
sample. Each such sample gets an `identity_clash` warning (`imported`) at
`SampleTbl.jsonl#<id>@materialID` that says why. A sample of the same name
and project that has a real material is another sample. A sample without a
project is still refused. `--catalog-from-repos` does the same for a record
that names a sample and a project but no material.

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

Catalog rows (samples, positions, levels, projects, identifiers, loads,
users, spectrometers) do not depend on the order in the same way. A row that
exists keeps every value it has, and a later source fills what the row lacks:
a position MetaData made without a sample gets its sample from the catalog
dump or, with `--catalog-from-repos`, from the project repository, whichever
is imported later. Where two sources give different values, the first stays.

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

Registers a source and prints its id. Nothing is stored if any check fails.
Registering the same source again (same kind, source and branch) keeps its id
and everything imported from it; it replaces the settings the cache holds for
it with those of the new command: `--reference-runs`, `--author-map`, and
the path it is read from. The time zone and `--catalog-from-repos` cannot
change (they decide what was imported): `add` with other values is refused.

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
  identifiers, positions, samples, projects, materials and spectrometers
  from the analysis records. Each identifier made up this way leaves one
  warning conflict (`synthesized`). Principal investigators exist only in
  the legacy MySQL database: an import from repositories alone has none.
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
  batches (`--batch` sets the batch size; default 500 commits, 2000 rows for
  a dump).
- **`--replay`.** Walks the source again from the start. What is already
  imported is left alone; analyses that were refused earlier are imported
  now. Use it after you have fixed the catalog (for example after importing
  the dump you had left out): the summary tells you so when
  `unknown_analysis` conflicts are pending. The conflicts it resolves become
  `superseded`. A replay does not resolve an `unparseable` conflict, and it
  refuses a source whose history was rewritten, as a plain run does.
- **`--all`** imports every source in order. A source that cannot be opened
  (its repository or dump is gone, its settings file in the cache is damaged
  or missing) is reported on stderr and skipped; the others are imported.
- **`--dry-run`.** Writes nothing and fetches nothing; prints how many rows a
  run would write. It needs an existing database and, for a url source, a
  mirror already in the cache.
- The conflict count in the progress lines and the summary is what the run
  leaves pending; a conflict it wrote and superseded in the same run is not
  counted.
- Exit code: 0 when finished or paused, 1 when a source finished with
  blocking conflicts pending, 2 on an error, and 2 when a source could not be
  opened (after the others were imported).

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

No command resolves a blocking conflict by decree: there is no "ignore". A
blocking conflict goes away (becomes `superseded`) when what it complains
about is mended in the source and imported, and until then `verify` is not ok
for that source. What mends each kind is in the table. A catalog dump is
never mended, but the importer's rules can change: `run --replay` of a dump
supersedes the pending conflicts of its rows that the dump, as it is read
now, no longer has (the refusal of a row that is now imported, a link a row
now keeps), and rewrites a pending one whose reason is no longer the reason.

| Kind | Meaning |
|---|---|
| `unparseable` | A file could not be read (the reason is in the detail: invalid JSON, a chronology line that cannot be read, content the reader did not expect), a path is not a file of a legacy repository, or a spectrometer file appeared after the analysis it belongs to. Blocking. A file that could not be read is superseded by the next run that imports a later commit with a readable version of that file, or one that deletes it; an unreadable version that follows a readable one is not. So the conflict stays, and `verify` stays not ok, for as long as the file is unreadable at the head of the branch: mend it in the source repository and run again. The other two reasons are never superseded: an unknown path and a late spectrometer file stay as they are. |
| `unknown_analysis` | A file was refused because its analysis is not in the store. Blocking. The detail says which case it is: (1) the catalog has no such identifier, or names no such spectrometer or extraction device: fix the catalog and `run --replay`; (2) the analysis was refused for another reason (its run id is taken: see its `identity_clash`): mend that, then `run --replay`; (3) the record of the analysis cannot be read: superseded, without a replay, by the run that imports a commit with a readable record (the analysis then starts at that commit); (4) the repository has files for an analysis but never its record: superseded by the run that imports the record, if one is ever committed; (5) a membership or a revision of an analysis another source has not imported yet: import that source, then `run --replay`. |
| `identity_clash` | Two things claim the same identity (a run id or position already holding another analysis). Blocking when something was refused. A warning when the row was imported without a broken optional link, or a sample under the placeholder material `unknown`, or when a catalog row that already exists could not take the values a source brings for it (`imported`; path `catalog-fill/<table>/<key>`, the store's reason in the detail), when the catalog row was made from the repositories (`synthesized`), or when an older version could not be placed behind a newer one (`late_revision_not_applied`). The last is blocking instead when the stored revision it is behind comes from a commit the repository no longer has (`"cause": "stored_commit_unknown"`): the history was rewritten after it was imported. |
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
   with the legacy ones. The analysis's own files are taken as of the
   interpreted age's commit in the repository's history; reference data
   (flux, production, chronology) as of that commit's time: for each, the
   last revision made at or before it, whatever was changed later. Each
   member is reported as `pass`, `pass on age only` (the file does not say
   which kind of error it holds), `fail` or `not comparable`, with the
   reason for the last: for example `other_source` (an analysis of another
   repository), `reference_not_yet_defined` (the flux, production or
   chronology was first written after the age was saved), `no_j`,
   `no_production`, `no_chronology` (there is none, or it had been removed by
   then). A failure is also stored as a `value_mismatch` conflict. The output ends with the largest
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

A source whose history was rewritten after it was imported is not verified:
`verify` fails with `history was rewritten`, as `run` does.

Exit code: 0 when every source checked is ok, 1 when one is not, 2 on an
error, when nothing is registered, or when a source could not be opened (the
others are still checked).

## 4. Credentials

A source url must not carry a secret: it would be written to the settings
file and the store, and shown by `status`. `elctl import add` refuses, before
it fetches anything and without printing the url,

- `scheme://user:password@host/...` on any scheme;
- any `user@` on `http` and `https`, where the user is often a token
  (`https://token@host/...`);
- a query string that names a `token`, `password`, `secret` or `key=`.

A user without a password on `ssh://` (`ssh://git@host/path`) and the
scp-like form `git@host:path` name an account, not a secret, and are allowed;
use them with an ssh agent.

For `http` and `https`, let git supply the credentials: the clone and the
fetch of a mirror run with your own git configuration (system and global), so
a credential helper works as it does for `git clone`:

```bash
git config --global credential.helper osxkeychain   # or manager, libsecret, store
```

The same goes for a url rewrite (`url.<base>.insteadOf`) or a proxy set there.
The import never prompts: without a working helper the fetch fails with git's
message. Only the `file`, `git`, `http`, `https` and `ssh` transports are
allowed, whatever the configuration says. Everything else the importer runs
(reading a repository or a mirror) ignores your git configuration.

The database url may hold a password for PostgreSQL; prefer a password file
or the environment your PostgreSQL client reads.

## 5. Known limits

These are things the importer does not do, or does differently from what you
might expect. Section 10 of the design spec has the full wording.

- **Edits made in the new application stay.** Once an item has been changed
  in the new application, the legacy source no longer updates it. A later
  edit to the same item in the legacy repository arrives as a warning
  (`late_revision_not_applied`) with its content kept in the conflict. The
  opposite holds for the first import of a source: until it has finished
  once, it overwrites the heads that already exist for the items it brings,
  whether a user made them or another source did. This is so whether the
  first import runs in one go or is stopped and resumed.
- **Several workstations.** When a repository is merged from several
  workstations and imported in more than one run, the revision history can
  lack intermediate revisions that one uninterrupted import of the final
  history would hold. The current values (heads) agree.
- **Merge order and bookmarks.** Git's order can put commits of a branch that
  has not been merged yet before an unrelated merge. A bookmark made from a
  git tag on that merge can then include a value from the unmerged branch;
  heads are right once the branch is merged. A git tag added later to a
  commit already imported becomes a bookmark only when a replay sees it, and
  it then bookmarks the values current at the time of that replay, not those
  at the tagged commit.
- **Three or more branches at once.** Where more than two lines of work are
  merged one after another, the current value of an item can differ, right
  after an intermediate merge, from what that merge's tree holds; it is
  right again once the last of the branches is merged.
- **After an unreadable version, a merge can repeat content.** A merge is
  compared with the version of a file the walk last saw. When that version
  could not be read, the merge can write content that is already imported
  again, as a new revision with the same values.
- **Analyses still being collected.** An analysis whose files are incomplete
  when an incremental run ends is imported as it stands (a synthetic
  collection); one longer uninterrupted import would have found it complete.
  The later files arrive as revisions. Within one run the wait is bounded
  too: an analysis still incomplete 20 commits after its record is imported
  with the files it has.
- **Renumbered while being collected.** An analysis whose run id is changed
  before its collection is complete is imported under the later run id, with
  no revision for the renumbering; the rewrite is kept in the provenance of
  its commit.
- **An unreadable first record.** An analysis whose record cannot be read is
  not imported until a commit brings a readable record; it then starts at
  that commit (a synthetic collection) with the files the repository has had
  for it. If one of those files could not be read either, its conflict keeps
  the kind and wording it got while the record was unreadable
  (`unknown_analysis`), and stays until the file is mended.
- **Several analyses in one commit.** Two analyses that swap run ids in one
  commit both become `identity_clash`. A collection in the same commit as the
  renumbering that frees its run id is refused until a replay. A record
  without a uuid that is renumbered reads as a different analysis.
- **Refused, then renumbered.** An analysis refused in one run, then
  renumbered, whose old run id another analysis took, is an `identity_clash`
  on replay.
- **The catalog's existing values win.** When a catalog row already exists in
  the store it keeps every value it has; a later source only fills what the
  row lacks. Verify cannot tell that a value of the dump was not applied
  because the row already had another. For a catalog, `ok` means the dump
  was imported, not that every column of every row holds the dump's value.
  When the store cannot take a value a row lacks (an identifier given a
  position another identifier holds, a blank or air identifier given a
  position, a spectrometer code another spectrometer has), the row stays as
  it is, the import goes on, and an `identity_clash` warning at
  `catalog-fill/<table>/<key>` records what was not applied and why.
- **Reference data that cannot say "removed".** When a position, a
  production, a chronology, a gains or a holder file, or a level's geometry
  is deleted from MetaData the importer writes a revision that says there is
  no value. Two kinds cannot say that: the link from a level to its
  production (a level dropped from `productions.json`) and the sensitivity
  of a spectrometer (an emptied or deleted list). They keep their old value
  as the current one; the removal is recorded only in the provenance note of
  the commit's changeset.
- **Ages are checked against the data as of the interpreted age.** A member
  imported from another source is not comparable: its history cannot be
  placed in this repository's. Reference data is taken by time, not by place
  in a history: the last revision of the flux, of the level's production
  link and the production it named, and of the chronology made at or before
  the interpreted age's commit time. This relies on the commit dates of the
  two repositories; where the MetaData commit that the age was computed with
  carries a later date than the interpreted age's commit, the member is
  `reference_not_yet_defined`, or is compared with the revision before it.
  Reference data that had been removed by then gives `no_j`, `no_production`
  or `no_chronology`, never an earlier value.
- **Spectrometer names are lower-cased**, as the legacy database and the
  MetaData repository name them (`Felix` in a record is `felix`); the
  spelling in the file is kept with the analysis.
- **Files MetaData holds that are not reference data** (scripts, experiment
  templates, documents) are ignored. They are not conflicts and are not
  counted.
- **`--catalog-from-repos` rows can depend on where runs were cut.** The
  catalog rows made from the records (which identifier sits at which
  position, which gets a position at all) are decided when each record is
  first imported; two repositories that claim one position, or runs stopped
  at different places, can leave different rows. Import a database dump when
  you have one.
- **One importer at a time.** Two `elctl import run` on one database at the
  same time are not supported; nothing stops them.
- **Windows.** The importer is not built or tested on Windows in CI (the
  store needs Qt, which the Windows job does not have).
- **Run logs** (`logs/*.logs.log`) are not imported; they count as accounted
  for.
