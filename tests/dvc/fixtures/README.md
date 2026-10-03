# Legacy pychron DVC fixtures and layout survey

Surveyed 2026-10-03 for the legacy ingestion plan
(`docs/superpowers/plans/2026-10-03-legacy-ingestion.md`, Task 1). This file
is the authority for the parsers of Tasks 9, 11, 12 and 13. Where it
disagrees with the plan's remembered layout, this file wins.

Every statement below cites either a fixture file in this directory, a path
in a public repo at a recorded commit, or a line of the legacy source.
Statements that rest on the source alone (no real file seen) are marked
**source only**. Things that could not be established are marked
**unconfirmed**.

Every file in this directory except this README is a byte-identical copy of
a git blob in a public repo. Do not reformat, re-indent or strip whitespace:
several files end lines with a trailing space (Python 2 `json.dump`) and none
of the JSON files end with a newline. `git hash-object <file>` must equal the
blob sha in the tables of section 2.

## 1. Sources

| Name | URL | Commit | Notes |
|---|---|---|---|
| pychron (legacy source) | https://github.com/NMGRL/pychron | `6ccadb4418c003c813d154504413ab3571bb1660` | Source line numbers below refer to this commit. Paths are relative to the repo root. |
| **IR1010** (real-data project fixture) | https://github.com/NMGRLData/IR1010 | `5283d6c85f3e2f57aedd6ad9d0418c7a7257bcd0` (branch `master`) | 166 commits, 39 analyses, one merge-free line of history. |
| Felix_blank180 (blank analysis) | https://github.com/NMGRLData/Felix_blank180 | `d831c6231d0d0cbbb6bc474f5ab296d25a47004a` (branch `master`) | 762 commits, two merge commits. |
| MetaData (meta repo) | https://github.com/NMGRLData/MetaData | `0ef8d84412ab2f7401c6bae86e0a821ccded72ed` (branch `master`) | 11067 commits. The head moves daily; pin this sha. |
| Cornudas (second interpreted-age format only) | https://github.com/NMGRLData/Cornudas | `e9510b8050cd387250a4af828fade0c4b3cca2cd` | Only one file taken. |

### 1.1 Why IR1010

IR1010 is the smallest public NMGRLData project repo (682 KB by the GitHub
API) that has a refit after collection, a blank change and an interpreted
age. Every public repo from 20 to 3000 KB was checked (202 repos); the only
other one with an interpreted-age file in that range is IR998 (2990 KB).
What IR1010 contains, in commit order (`git log --reverse`):

| Commit | Author date | Message | Files |
|---|---|---|---|
| `4d73069` | 2018-02-16 | `Initial commit` | adds `README.md` |
| 39 x 4 commits | 2018-02-19 .. 02-20 | collection sequences (section 4) | |
| `aef7f63` | 2018-06-05T14:57:22-06:00 | `<ISOEVO> fits=L1(Average),...,Ar40(Auto)` | modifies 39 `660/intercepts/*.inte.json` |
| `8ad0516` | 2018-06-05T14:57:25-06:00 | same message as `aef7f63` | modifies 39 `660/baselines/*.base.json` |
| `0e87323` | 2018-06-05T14:58:58-06:00 | `<BLANKS> auto update blanks, fits=Ar36(Bracketing Interpolate),...` | modifies 39 `660/blanks/*.blan.json` |
| `6564a23` | 2018-06-05T15:09:33-06:00 | `<ICFactor> auto update ic_factors, fits=L2(CDD)(Bracketing Interpolate)` | modifies 39 `660/icfactors/*.icfa.json` |
| `0579695` | 2018-06-05T15:10:51-06:00 | `<ICFactor> auto update ic_factors, fits=H1(Bracketing Interpolate)` | modifies 39 `660/icfactors/*.icfa.json` |
| `574fee6` | 2018-06-05T15:16:17-06:00 | `<TAG> omit   66052-01E - 66052-01M` | adds 9 `660/tags/*.tags.json` |
| `fb787e1` | 2018-06-05T15:17:44-06:00 | `<IA> added interpreted age 01` | adds `660/ia/52.ia.json` |
| `ba9367e` | 2018-06-07T14:20:47-06:00 | `<TAG> invalid 66049-01A` | adds `660/tags/49-01A.tags.json` |
| `5283d6c` | 2021-02-08T10:35:53-07:00 | `<ICFactor> bulk edit scaled icfactor by 0.9932318104906938` | modifies 38 `660/icfactors/*.icfa.json` |

IR1010 has no blank analyses: its blanks were measured into a separate
per-spectrometer repo. The blank fixture is therefore `bu-FD-F-789` from
Felix_blank180, the blank that IR1010's `<BLANKS> preceding bu-FD-F-789`
commit (`175b549`) names for the fixture unknown.

IR1010 has no git tags and one extra remote branch, `ic_factor_fix`, which
points at the same commit as `master`.

**Warning for age parity (Task on verify).** The interpreted age was saved
at `fb787e1` (2018-06-05). Commit `5283d6c` (2021) later rescaled the
`L2(CDD)` IC factor of 38 analyses, including the fixture unknown. Ages
recomputed from the files at the repo head will therefore not equal the ages
stored in `660/ia/52.ia.json`. Only the files as of `fb787e1` can reproduce
them. The flux in the meta repo can also have changed since
(`NM-293/G.json` was last touched in 2025, section 6.1).

## 2. Files in this directory

### 2.1 `project/IR1010/` — unknown `66052-01E`

uuid `15fb3686-4aed-40c1-8987-e73a8a52b434`. All paths are relative to the
IR1010 repo root and are kept unchanged under `project/IR1010/`.

| Path | Blob sha | Kind |
|---|---|---|
| `660/52-01E.json` | `28f87b22240f16c6bbf49e49559b2026b142b9dc` | analysis |
| `660/.data/52-01E.dat.json` | `e12b7bc470688eefb68b8ee9021c961261336fe6` | raw data |
| `660/extraction/52-01E.extr.json` | `5bb2d2d1e9908e83dd6c428a752c57b8a81c5273` | extraction |
| `660/intercepts/52-01E.inte.json` | `6e1edd4aab728663ed2f6a60967fb32a8faa90fa` | intercepts |
| `660/baselines/52-01E.base.json` | `3339f6d8074238cf10ab91d4f24d222f38105ea1` | baselines |
| `660/blanks/52-01E.blan.json` | `4663412c86e577c4be4b9664b3e4142fe053ac24` | blanks |
| `660/icfactors/52-01E.icfa.json` | `ee481e9aad2a33ee9228600198d7c7632a489fb3` | icfactors |
| `660/tags/52-01E.tags.json` | `1ee4f0f2c30dc4dd75084f03387dad81c7a7edcb` | tags |
| `6a9b4615cd24138b6ce541f75240dc4091378bb1.json` | `a7bbe20ddc1f5838df00de1416eb5a36be7a3323` | spectrometer settings named by the analysis's `spec_sha` |
| `660/peakcenter/52-01A.peak.json` | `10b3b00cc981cae1ec4cfbfb0a0fc45b303abd3b` | peak center. **Belongs to `66052-01A`, not to the fixture unknown** (`66052-01E` has no peak center; only 5 of the 39 analyses do). Included so every observed kind has a real example. |

These are all the files legacy pychron wrote for `66052-01E`
(`git log -- '660/*52-01E*'` at the head lists no others). The copies are the
head versions, so intercepts, baselines, blanks and icfactors are the
*reviewed* versions, not the collection defaults. The collection versions
are `git show 1f47830:660/intercepts/52-01E.inte.json` and so on
(section 4.2).

### 2.2 `project/Felix_blank180/` — blank `bu-FD-F-789`

uuid `7834c4f8-f3b8-4aac-b586-53f7cb8d3d5c`, `analysis_type`
`blank_unknown`. Paths relative to the Felix_blank180 repo root.

| Path | Blob sha | Kind |
|---|---|---|
| `bu-FD/-F-789.json` | `f69445e840aae52e55902bfb7283d4713048d2c1` | analysis |
| `bu-FD/.data/-F-789.dat.json` | `bb9a12fdd5bdc040e1771b694de032bc56bcf9a4` | raw data |
| `bu-FD/extraction/-F-789.extr.json` | `21b54e46b1ec6dca0c61b2f7d33002c29dc1b90c` | extraction |
| `bu-FD/intercepts/-F-789.inte.json` | `5e6bc22c09c068b86546237347af961e54318d44` | intercepts |
| `bu-FD/baselines/-F-789.base.json` | `ca7693d78a7fd47c20d02f5b488193653936be85` | baselines |
| `bu-FD/blanks/-F-789.blan.json` | `09c134fb4b7dc6983643b16ca422634a2669c144` | blanks |
| `bu-FD/icfactors/-F-789.icfa.json` | `d41dcb4f2f1bfde42c2b40a7631f9b566704d6a5` | icfactors |
| `fad234d16fee6c00a96e96e15edccd97faba9521.json` | `a8720e8ee4e8043a482a82dcfa30f3fb96f93545` | spectrometer settings named by `spec_sha` |

This blank was never refit: all seven files are the collection versions
(four commits `61f0111`, `3e10075`, `34ddfbf`, `40f2578`, all
2018-02-19T23:29:00-07:00). Note the file names begin with `-`.

### 2.3 `ia/` — interpreted ages

| Path here | Source repo and path | Blob sha | Format |
|---|---|---|---|
| `ia/IR1010/660/ia/52.ia.json` | IR1010 `660/ia/52.ia.json` | `aaa8b3ac1b791db4e7fc7ac4ce54140c4ac18b6c` | flat (2018) |
| `ia/Cornudas/694/ia/21_00000.ia.json` | Cornudas `694/ia/21_00000.ia.json` | `b13f175efec984a4c12f174c4c7da30d6b50dd44` | nested (2022) |

The brief asked for one interpreted-age file. Two are kept because two
incompatible formats exist (section 5) and a parser needs a real example of
each. The Cornudas file comes from another repo because IR1010 has only the
flat format.

### 2.4 `meta/` — from MetaData

| Path | Blob sha | What |
|---|---|---|
| `NM-293/G.json` | `99558c94a9b426161c59c3d42b396eff772de1ac` | level file; the fixture unknown is identifier `66052`, position 16 |
| `NM-293/productions.json` | `9f2f53e5b2811032de4cb6ca3ce5f2b5f9381106` | level name to production name |
| `NM-293/productions/Triga_PR.json` | `ca52f39e9dd7e96954a19869e23ce356191f535d` | production ratios |
| `NM-293/chronology.txt` | `8036ca8d2fac9fee884eefaf8f1a9a23ba32209e` | irradiation chronology |
| `spectrometers/jan.gain.json` | `9e26dfeeb6e641a33dae4961196235bdb965b21b` | gains file. Content is `{}` (2 bytes). The only gain files in the repo are `jan.gain.json` and `obama.gain.json`, both `{}`; there is no `felix.gain.json`. |
| `spectrometers/felix.sens.json` | `0792ea6a0f471bfaeaa8e02712f7c04493f68026` | sensitivity list for the fixture's spectrometer |
| `irradiation_holders/24_hole.txt` | `9514dc360662c5c673e4d358755421b1cebb591c` | one irradiation holder. **Unconfirmed** that NM-293 used this holder: the holder name is only in MySQL `LevelTbl.holder`. |
| `load_holders/37-hole.txt` | `736334304fa414e2c92fffa6d673fcbac0871feb` | load holder named by the fixtures' extraction `tray` (`"37-hole"`). Not asked for by the brief; 539 bytes. |

## 3. Path to kind

### 3.1 How a path is built

`pychron/dvc/__init__.py:178-259` (`_analysis_path`):

1. `name` is the analysis uuid or the runid, with `:` replaced by `_`
   (line 188).
2. `name` is split into a directory prefix `name[:n]` and a tail `name[n:]`
   (`subdirize`, `pychron/core/helpers/filetools.py:31-53`).
3. Without a modifier the file is `<prefix>/<tail>.json`.
4. With a modifier the file is `<prefix>/<modifier>/<tail>.<modifier[:4]>.json`
   (lines 227-241). If the modifier starts with `.` no extra dot is added
   (line 237-238), so modifier `.data` gives `<prefix>/.data/<tail>.dat.json`.

**The prefix length is not fixed.** Lengths seen:

| n | When | Real example |
|---|---|---|
| 3 | numeric identifier runid (`__init__.py:207`) | IR1010 `660/52-01E.json` for runid `66052-01E` |
| 5 | runid with more than one `-` whose first part has 2 or more characters (`__init__.py:208-213`) | Felix_blank180 `bu-FD/-F-789.json` for runid `bu-FD-F-789` |
| 4 | runid with more than one `-` whose first part has 1 character (`__init__.py:210-211`) | NMGRLData/FractionatedRes `a-01/logs/-F-2505.logs.log` for runid `a-01-F-2505` (path listing only) |
| 2 | uuid name. Current writer: `use_uuid_path_name = True` and `force_sublen=2` (`dvc_persister.py:88`, `:975-987`) | Cornudas (section 1): `00/c63177-34d6-4de1-9e2b-e63b1a149436.json` for uuid `00c63177-34d6-4de1-9e2b-e63b1a149436` (runid `69422-01D`, collected 2022-01-23) |
| 5, 3 | uuid name, read fallbacks (`__init__.py:202-203`) | **source only** |

Consequences for `classify_path`:

- The key of an analysis is `<prefix> + <tail>`: concatenate the first path
  segment and the file stem with the suffix removed. Never assume a length.
- The key is a **runid** in older repos and a **uuid** in newer ones
  (Cornudas, 2021-2022). Decide with the uuid pattern at
  `__init__.py:172-175`. When the key is a uuid the runid is not in the path
  at all; it comes from `identifier`, `aliquot` and `increment` inside the
  analysis JSON.
- A runid in a path has had `:` replaced by `_`. **Unconfirmed** whether any
  real runid contains `:`.
- One repo can mix prefix lengths. Cornudas has 2-character uuid
  directories for analyses and 3-character directories (`694/ia/`) for
  interpreted ages.

### 3.2 The table

`<p>` is the prefix directory, `<t>` the tail. "Seen" names a real file.

| Kind | Path pattern | Seen | Source |
|---|---|---|---|
| analysis | `<p>/<t>.json` | `project/IR1010/660/52-01E.json` | `dvc_persister.py:881-882` |
| raw data | `<p>/.data/<t>.dat.json` | `project/IR1010/660/.data/52-01E.dat.json` | `dvc_persister.py:898-907`, `__init__.py:56` |
| intercepts | `<p>/intercepts/<t>.inte.json` | `project/IR1010/660/intercepts/52-01E.inte.json` | `dvc_persister.py:884-885`, `__init__.py:61` |
| baselines | `<p>/baselines/<t>.base.json` | `project/IR1010/660/baselines/52-01E.base.json` | `dvc_persister.py:891-892`, `__init__.py:58` |
| blanks | `<p>/blanks/<t>.blan.json` | `project/IR1010/660/blanks/52-01E.blan.json` | `dvc_persister.py:888-889`, `__init__.py:59` |
| icfactors | `<p>/icfactors/<t>.icfa.json` | `project/IR1010/660/icfactors/52-01E.icfa.json` | `dvc_persister.py:894-895`, `__init__.py:60` |
| tags | `<p>/tags/<t>.tags.json` | `project/IR1010/660/tags/52-01E.tags.json` | `__init__.py:57`, `util.py:41-59` |
| extraction | `<p>/extraction/<t>.extr.json` | `project/IR1010/660/extraction/52-01E.extr.json` | `dvc_persister.py:265-266` |
| peak center | `<p>/peakcenter/<t>.peak.json` | `project/IR1010/660/peakcenter/52-01A.peak.json` | `dvc_persister.py:944`, `__init__.py:62` |
| monitor | `<p>/monitor/<t>.moni.json` | none in about 320 public repos looked at. **source only** | `dvc_persister.py:914` |
| cosmogenic | `<p>/cosmogenic/<t>.cosm.json` | none. **source only** | `dvc_analysis.py:652-656`, `__init__.py:63` |
| interpreted age | `<p>/ia/<t>.ia.json` where `<p><t>` is `<identifier>` (2018) or `<identifier>_<NNNNN>` (later) | `ia/IR1010/660/ia/52.ia.json`, `ia/Cornudas/694/ia/21_00000.ia.json` | `dvc.py:2162-2181`, `func.py:145-152` |
| spectrometer settings | `<40 hex>.json` at the repo root | `project/IR1010/6a9b4615cd24138b6ce541f75240dc4091378bb1.json` | `dvc_persister.py:298-302`, `:930-940` |
| run log | `<p>/logs/<t>.logs.log` | NMGRLData/IR1003 `656/logs/02-02A.logs.log` (path listing only, content not read) | `dvc_persister.py:465-476` |
| frozen production | `<irradiation>.<level>.production.json` at the repo root | NMGRLData/Gootee01286 `NM-312.F.production.json` (path listing only) | `__init__.py:318-325` |
| frozen production (per analysis) | `<p>/productions/<t>.prod.json` | none. **source only** | `dvc.py:751` |
| frozen flux | `<irradiation>.json` at the repo root | none. **source only**. Not distinguishable from a spectrometer file by suffix; the stem is not 40 hex. | `meta_repo.py:102-110` |
| repo files | `README.md` at the repo root | IR1010 `README.md` | |

Corrections to the layout the plan expected:

1. The raw data suffix is **`.dat.json`**, not `.data.json`.
2. The prefix is not always `runid[:3]` (section 3.1).
3. `cosmogenic` is not in `PATH_MODIFIERS` (`__init__.py:65-68`); it is
   written only by `dump_cosmogenic`.
4. Kinds the plan did not list but which exist in real repos: spectrometer
   settings, run log, root-level frozen production, interpreted age.

`reduction/` root (**source only**). The current source can also place tags
and interpreted ages under `reduction/<p>/tags/` and `reduction/<p>/ia/`
(`__init__.py:39-41`, `:266-297`, `func.py:150`). No public repo looked at
has a `reduction/` directory. `temp/` under the repo root is used for
temporary copies (`__init__.py:194-197`).

## 4. Collection commits

### 4.1 What the source does

`pychron/dvc/dvc_persister.py:340-407`, in this order, all on one branch
(the repo's current branch, or `data_collection` when
`use_data_collection_branch` is set, lines 344-353):

| # | Message | Files staged | Source lines |
|---|---|---|---|
| 1 | `<COLLECTION>` (exactly; the tag is the argument `commit_tag`, default `"COLLECTION"`) | the spectrometer file `<spec_sha>.json` if it is new, then every existing file for the modifiers `NPATH_MODIFIERS` = analysis, `.data`, `tags`, `peakcenter`, `extraction`, `monitor` | 355-370; `__init__.py:69` |
| 2 | `<ISOEVO> default collection fits` | `intercepts`, `baselines` (one commit for both; skipped if neither exists) | 375-390 |
| 3 | `<BLANKS> preceding <previous blank runid>` | `blanks` | 394-405 |
| 4 | `<ICFactor> default` | `icfactors` | 394-405 |

The tag list is `HISTORY_TAGS` at `__init__.py:44-54`: `TAG`, `ISOEVO`,
`BLANKS`, `ICFactor`, `DEFINE EQUIL`, `MASS SPEC REDUCED`, `COLLECTION`,
`IMPORT`, `MANUAL`. Case matters: it is `ICFactor`, not `ICFACTOR`.

### 4.2 What the fixture shows

For `66052-01E` (IR1010), all authored by `felix <nmgrluser@gmail.com>` at
2018-02-20T00:27:10-07:00:

| Commit | Message | Adds |
|---|---|---|
| `941b7347d9d21e591d5930f47f3d436fa058c51c` | `<COLLECTION>` | `660/52-01E.json`, `660/.data/52-01E.dat.json`, `660/extraction/52-01E.extr.json` |
| `1f478308564edabaeab1db6090c499bb2a62eebf` | `<ISOEVO> default collection fits` | `660/baselines/52-01E.base.json`, `660/intercepts/52-01E.inte.json` |
| `175b5494832bf38f6a154a83fb193cbe45f4d7b5` | `<BLANKS> preceding bu-FD-F-789` | `660/blanks/52-01E.blan.json` |
| `dd836551c9b799f353be228222fbceda8529c00f` | `<ICFactor> default` | `660/icfactors/52-01E.icfa.json` |

The first analysis in the repo (`66049-01A`, commit `3ce9723`) also added
`660/peakcenter/49-01A.peak.json` and
`fad234d16fee6c00a96e96e15edccd97faba9521.json` in its `<COLLECTION>` commit.
A spectrometer file is added only by the first analysis that uses it; the
fixture unknown's `6a9b4615...json` was added by an earlier analysis.

The four commits of one sequence can carry author times one second apart
(IR1010 `b79fa8f` at 20:44:42 and `e88931e` at 20:44:43). The author of a
collection commit is the instrument account, not the analyst.

### 4.3 Later commits (not collection)

Message formats seen or in the source. The file suffix, not the message,
decides the kind.

| Message | Touches | Where |
|---|---|---|
| `<ISOEVO> fits=<k>(<fit>),...` | intercepts in one commit, then baselines in a second commit with the same message | IR1010 `aef7f63`, `8ad0516` (2018) |
| `<ISOEVO>.intercepts fits=...` and `<ISOEVO>.baselines fits=...` | the same two commits, newer message format `<TAG>.<modifier> <msg>` | Felix_blank180 `e11348f`, `d831c62` (2025); `pychron/pipeline/nodes/persist.py:91-101`, `:146-149` |
| `<BLANKS> auto update blanks, fits=...`; newer `<BLANKS>.blanks ...` | blanks | IR1010 `0e87323`; Cornudas has 6 `<BLANKS>.blanks` commits; `persist.py:189-210` |
| `<ICFactor> auto update ic_factors, fits=...`; newer `<ICFactor>.icfactors ...` | icfactors | IR1010 `6564a23`; Cornudas has 9 `<ICFactor>.icfactors` commits; `persist.py:219-241` |
| `<ICFactor> bulk edit <msg>` | icfactors | IR1010 `5283d6c`; `pychron/pipeline/nodes/bulk_edit.py:200` |
| `<TAG> <tag padded to 6> <first runid> - <last runid>` or `<TAG> <tag> <runid>` | adds or modifies tags files | IR1010 `574fee6`, `ba9367e`; `dvc.py:1966`, `:2146-2154` |
| `<IA> added interpreted age <name>` (2018), `<IA> added interpreted ages <name> <identifier> <sample>,...` (later) | adds ia files | IR1010 `fb787e1`; Cornudas `c919ed7`; `dvc.py:1656-1668` |
| `<DEFINE EQUIL> ...`, then `<ISOEVO> modified by DEFINE EQUIL` | analysis-level paths, then intercepts | **source only**, `persist.py:125-130` |
| `<MANUAL> reverted to non manually edited` | intercepts, blanks, baselines, icfactors | **source only**, `dvc.py:803-817` |
| `<MASS SPEC REDUCED> <msg>` | | **source only**, `pychron/pipeline/nodes/mass_spec_reduced.py:234` |
| `<IMPORT> initial` | | **source only**, `pychron/dvc/pychrondata_transfer_helpers.py:264` |
| `<EDIT> RunID`, `<EDIT>` | | **source only**, `bulk_edit.py:223-229` |
| `<SYNC> Synced repository with database <url>` | analysis JSON (sample, project, material, irradiation fields rewritten) | **source only**, `dvc.py:611-618`, `:622-666` |
| `<PR_FREEZE>` | per-analysis frozen productions | **source only**, `dvc.py:756` |
| `<CSV> ...` | csv datasets | **source only**, `dvc.py:1058-1062` |
| `<COLLECTION> log` | run log | **source only**, `dvc_persister.py:475` |
| `Transferred analyses to <dest>` / `Transferred analyses from <src>` | all files of the moved analyses; no tag | **source only**, `dvc.py:668-685`, `:2115-2136` |
| `added repository association` | copies of all files of an analysis into a second repo; no tag | **source only**, `dvc.py:1671-1691` |
| `Initial commit`, `Merge branch 'master' of ...` | | IR1010 `4d73069`; Felix_blank180 `c365bd7`, `1904c06` |

Tags in git. The current source has `USE_GIT_TAGGING = False`
(`__init__.py:37`): `tag_items` then writes no tags file and makes no
`<TAG>` commit (`dvc.py:1932-1966`); the tag is stored only in MySQL
`AnalysisChangeTbl.tag`. The 2018 repos do have tags files and `<TAG>`
commits. **Unconfirmed** when git tagging was turned off. An analysis with no
tags file has no tag in git.

Branches. Five of the public repos looked at have a `data_collection`
branch (Azores, Canaries, Cornudas, SwansonB, Timescale). No public repo
looked at has a git tag.

## 5. JSON keys

`U` = `project/IR1010/660/...` (unknown), `B` =
`project/Felix_blank180/bu-FD/...` (blank).

### 5.1 Analysis file (`<p>/<t>.json`)

Written by `dvc_persister.py:805-882` from `META_ATTRS`
(`pychron/pychron_constants.py:540-557`).

| Field | Key | Fixture value (U / B) | Notes |
|---|---|---|---|
| uuid | `uuid` | `"15fb3686-4aed-40c1-8987-e73a8a52b434"` / `"7834c4f8-f3b8-4aac-b586-53f7cb8d3d5c"` | lower-case, with dashes |
| identifier (labnumber) | `identifier` | `"66052"` / `"bu-FD-F"` | string |
| aliquot | `aliquot` | `1` / `789` | integer |
| increment (step) | `increment` | `4` / `null` | integer, 0-based: 0 = `A`, 4 = `E`, 26 = `AA` (`pychron/core/utils.py:71-83`). `null` means no step. The runid is `<identifier>-<aliquot as %02d><step letters>` (`pychron/experiment/utilities/runid.py:33-55`). |
| analysis type | `analysis_type` | `"unknown"` / `"blank_unknown"` | the reader maps `"sample"` and empty to `"unknown"` (`dvc_analysis.py:179-180`) |
| timestamp | `timestamp` | `"2018-02-20T00:27:08.852603"` / `"2018-02-19T23:28:58.784926"` | **naive local time, no offset** (`datetime.now().isoformat()`, `dvc_persister.py:309-312`, `:812`). The `<COLLECTION>` commit of U is authored at `2018-02-20T00:27:10-07:00`, which confirms local time. The legacy reader accepts `%Y-%m-%dT%H:%M:%S`, `%Y-%m-%dT%H:%M:%S.%f` and `%Y-%m-%d %H:%M:%S` (`dvc_analysis.py:165-171`). |
| time zero | `time_zero_timestamp` | absent in both | **source only** (`dvc_persister.py:813-814`), naive |
| mass spectrometer | `mass_spectrometer` | `"Felix"` / `"Felix"` | capitalized here; meta repo file names are lower case (`felix.sens.json`) |
| extract device | not in this file | | it is `extract_device` in the **extraction** file (section 5.7) |
| tag | not in this file | | tags file (5.6) or MySQL |
| repository | `repository_identifier` | `"IR1010"` / `"Felix_blank180"` | |
| sample metadata | `sample`, `material`, `project`, `irradiation`, `irradiation_level`, `irradiation_position` | `"SB15-03"`, `"Feldspar"`, `"IR1010"`, `"NM-293"`, `"G"`, `16` / `"blank_unknown"`, `"Blank"`, `"REFERENCES"`, `"NoIrradiation"`, `"A"`, `22` | a copy made at collection; `<SYNC>` commits rewrite it |
| analyst | `username`, `analyst_name` | `"MHeizler"` | |
| isotopes | `isotopes.<iso>.detector`, `.name` | `Ar36` on `L2(CDD)`, `Ar37` `L1`, `Ar38` `AX`, `Ar39` `H1`, `Ar40` `H2` | newer files add `units`, `serial_id` |
| detectors | `detectors.<det>.deflection`, `.gain` | | |
| spectrometer file | `spec_sha` | `"6a9b4615cd24138b6ce541f75240dc4091378bb1"` / `"fad234d16fee6c00a96e96e15edccd97faba9521"` | names `<spec_sha>.json` at the repo root |
| meta repo commit | `commit` | `"c27fcc912f0b5dc6e984fd84cdf21a21424f7c98"` | still `commit` in the 2022 Cornudas file. The current source writes `meta_commit` and `data_collection_commit` instead (`dvc_persister.py:876-879`, **source only**) |
| scripts | `measurement`, `extraction`, `post_measurement`, `post_equilibration` | script file names | note `extraction` here is a script name, not the extraction record |
| other | `comment`, `source.emission`, `source.trap`, `conditionals`, `tripped_conditional`, `environmental`, `experiment_queue_name`, `queue_conditionals_name`, `intensity_scalar`, `whiff_result`, `laboratory`, `instrument_name`, `acquisition_software`, `data_reduction_software` | | |

Keys present in a 2022 file (Cornudas
`00/c63177-34d6-4de1-9e2b-e63b1a149436.json`, not copied here) and absent in
the 2018 fixtures: `arar_mapping`, `collection_version`, `experiment_type`,
`grainsize`, `hops`, `latitude`, `lithology`, `lithology_class`,
`lithology_group`, `lithology_type`, `longitude`, `note`,
`principal_investigator`, `rlocation`, `unit`. No 2018 key is missing from
it. Its `timestamp` is `"2022-01-23T20:31:24.774651"`, still naive.

### 5.2 Raw data (`<p>/.data/<t>.dat.json`)

`dvc_persister.py:897-907`. Top-level keys in the fixtures: `format`
(`">ff"`), `encoding` (`"base64"`), `commit`, `signals`, `baselines`,
`sniffs`. The 2022 Cornudas file has the same six keys. The current source
writes `data_collection_commit` instead of `commit` (**source only**).

- `signals[]` and `sniffs[]`: `{"isotope", "detector", "blob"}`.
- `baselines[]`: `{"detector", "blob"}` (no isotope; one per detector).
- `blob` is standard base64 of a byte string of consecutive 8-byte records,
  each a big-endian float32 time followed by a big-endian float32 intensity
  (`struct.pack(">ff", x, y)`, `pychron/processing/isotope.py:109-117`;
  `pychron/core/helpers/binpack.py:28-33`).

Checked on `U/.data/52-01E.dat.json`: `signals` `Ar40`/`H2` decodes to
2720 bytes = 340 points, first `(27.734310150146484, 9.958504676818848)`,
last `(384.5098876953125, 11.088024139404297)`; this matches `n: 340` in
the intercepts file. Each baseline is 480 bytes = 60 points
(first time 401.80364990234375). Each sniff is 200 bytes = 25 points (first
time 0.5804991722106934). Sniff, signal and baseline times lie on one clock
in seconds. **Unconfirmed** what instant is zero.

### 5.3 Intercepts (`<t>.inte.json`)

Object keyed by **isotope name** (`Ar36` .. `Ar40`). Per isotope:

| Field | Key | Notes |
|---|---|---|
| value | `value` | float |
| error | `error` | float |
| fit | `fit` | `"Parabolic"`, `"Linear"`, ...; case varies |
| error type | `error_type` | `"SEM"` |
| outlier filter | `filter_outliers_dict.filter_outliers`, `.iterations`, `.std_devs` | |
| counts | `n`, `fn` | present only after a review |
| flags | `reviewed`, `include_baseline_error` | present only after a review |
| manual edit | `manual_value`, `use_manual_value`, `manual_error`, `use_manual_error` | **source only**, `dvc.py:782-801` |

Collection default written at `dvc_persister.py:787-793` has only `fit`,
`error_type`, `filter_outliers_dict`, `value`, `error` (see
`B/intercepts/-F-789.inte.json`).

### 5.4 Baselines (`<t>.base.json`)

Object keyed by **detector name** (`H2`, `L2(CDD)`, ...), same per-entry
keys as intercepts. In `U/baselines/52-01E.base.json` the reviewed detectors
(`AX`, `H1`, `L1`) have `n`, `fn`, `reviewed`, `include_baseline_error` and
`"fit": "Average"`; the untouched ones (`H2`, `L2(CDD)`) have only the
collection keys and `"fit": "average"`. The current source also writes
`modifier_value` and `modifier_error` (`dvc_persister.py:777-785`,
**source only**).

### 5.5 Blanks (`<t>.blan.json`) and IC factors (`<t>.icfa.json`)

Blanks: object keyed by **isotope name**. Keys `value`, `error`, `fit`,
`error_type`, `references`, and `reviewed` after a review.

- Collection default (`B/blanks/-F-789.blan.json`): `"fit": "previous"`,
  `references: [{"record_id": "bc-02-F-696", "exclude": false}]` (no uuid),
  `dvc_persister.py:795-803`.
- Reviewed (`U/blanks/52-01E.blan.json`): `"fit": "Bracketing Interpolate"`,
  `references: [{"record_id", "uuid", "exclude"}]` with seven entries.
  `exclude` is the **string** `"ok"` here and the **boolean** `false` in the
  default. Do not assume a type.

IC factors: object keyed by **detector name**. Keys `value`, `error`, `fit`,
`references`, `reviewed`.

- Collection default (`B/icfactors/-F-789.icfa.json`): `value` 1.0, `error`
  1e-20, `"fit": "default"`, `references: []`
  (`dvc_persister.py:764-769`).
- `U/icfactors/52-01E.icfa.json` has three shapes in one file: default
  (`AX`, `H2`, `L1`); reviewed `H1` with `"fit": "Bracketing Interpolate"`
  and a seven-entry `references` list; and bulk-edited `L2(CDD)` with
  `"fit": "bulk_edit"`, an extra key `scalar`, and
  **`"references": ""`** (an empty string, not a list;
  `pychron/dvc/__init__.py:308-315` returns `""` for no references).
- Other keys in the source only: `standard_ratio`, `source_correction`
  (`dvc_analysis.py:641-649`).

### 5.6 Tags (`<t>.tags.json`)

`U/tags/52-01E.tags.json` is `{"name": "omit", "note": "", "subgroup": ""}`
(`pychron/dvc/util.py:49-59`). `name` is the tag.

### 5.7 Extraction (`<t>.extr.json`)

`dvc_persister.py:176-267`, `EXTRACTION_ATTRS`
(`pychron_constants.py:514-532`).

| Field | Key | Fixture value (U / B) |
|---|---|---|
| extract device | `extract_device` | `"Fusions Diode"` / `"Fusions Diode"` |
| extract value, units | `extract_value`, `extract_units` | `4.0`, `"watts"` / `0.0`, `""` |
| durations | `extract_duration`, `cleanup_duration` | `40.0`, `60.0`. Older files may use `duration` and `cleanup` (`dvc_analysis.py:183-195`, **source only**). |
| tray | `tray` | `"37-hole"` |
| positions | `positions[]` with `position`, `x`, `y`, `z`, `is_degas` | `position` is the **string** `"25"` in U and `""` in B; it is the integer `4` in the 2022 Cornudas file |
| other | `weight`, `beam_diameter`, `pattern`, `ramp_duration`, `ramp_rate`, `measured_response`, `requested_output`, `setpoint_stream`, `snapshots`, `videos`, `grain_polygon_blob`, `commit` | |

There is no timestamp in the 2018 extraction files. The current source
writes a naive `timestamp` (`dvc_persister.py:264`); the 2022 Cornudas file
(`00/extraction/c63177-34d6-4de1-9e2b-e63b1a149436.extr.json`) does not have
one. That file adds `load_name`, `load_holder`,
`cryo_temperature`, `light_value`, `pre_cleanup_duration`,
`post_cleanup_duration`, `extraction_context`, `grain_polygons`.

### 5.8 Peak center, spectrometer file

Peak center (`project/IR1010/660/peakcenter/52-01A.peak.json`): `fmt`
(`">ff"`), `reference_detector`, `reference_isotope`, and one object per
detector with `low_dac`, `center_dac`, `high_dac`, `low_signal`,
`center_signal`, `high_signal` (may be `null`) and `points` (base64 `>ff`
pairs of DAC and signal). Newer files add `interpolation`, `resolution`,
`low_resolving_power`, `high_resolving_power` (`dvc_persister.py:942-973`).
An older form with a top-level `data` key exists in the reader
(`dvc_analysis.py:665-672`, **source only**).

Spectrometer file (`<spec_sha>.json`): `spectrometer`, `gains`,
`deflections`; newer files add `settings` (`dvc_persister.py:930-940`). The
file name is a sha1 over those dictionaries (`dvc_persister.py:74-81`), not
a git sha.

### 5.9 Interpreted ages: where per-analysis ages live

Both formats have a top-level **`analyses`** array, one object per analysis,
with `uuid`, `record_id` (runid), **`age`**, **`age_err`**, `age_err_wo_j`,
`kca`, `kca_err`, `kcl`, `kcl_err`, `radiogenic_yield`, `plateau_step`,
`tag`. This is where verify gets its per-analysis legacy ages. The fixture
unknown appears in `ia/IR1010/660/ia/52.ia.json` as
`analyses[4]`: `record_id` `"66052-01E"`, `age` `24.03351976363802`,
`age_err` `0.9085947707302583`, `tag` `"omit"`.

Units: the nested format has `analyses[].age_units` (`"Ma"`); the flat
format has only the top-level `display_age_units`.

The two formats differ everywhere else:

| | Flat (2018), `ia/IR1010/...` | Nested (2022), `ia/Cornudas/...` |
|---|---|---|
| identifier | top-level `identifier` (`"66052"`) | **no identifier key**. Take it from the path (`694` + `21` = `69421`) or from `analyses[].record_id`. |
| name, uuid | top-level `name`, `uuid` | top-level `name`, `uuid` |
| preferred age | top-level `age`, `age_err` (both `0.0` in the fixture), `preferred_kinds[]` with `attr`, `kind`, `value`, `error`, `error_kind` | `preferred.age`, `preferred.age_err`, `preferred.preferred_kinds[]` (adds `unit`, `weighting`) |
| ages by kind | top-level `ages.{integrated,isochron,plateau,weighted}_age` and `_err` | `preferred.ages...` |
| constants | top-level `arar_constants` | `preferred.arar_constants` |
| mswd, n | top-level `mswd`, `nanalyses` | `preferred.mswd`, `preferred.nanalyses` |
| sample | top-level `sample`, `material`, `project`, `irradiation`, `macrochron` | `sample_metadata.{sample, material, project, irradiation, irradiation_level, irradiation_position, ...}` |
| per-analysis extras | none | `rundate` (naive), `extract_value`, `age_units`, `radiogenic_yield_err`, and per-isotope `{value, error}` under `baseline_corrected_intercepts`, `blanks`, `icfactors`, `ic_corrected_values`, `interference_corrected_values` |
| session | none | `session_metadata.date` (naive), `collection_metadata.instrument` |

The nested format is what the current source writes
(`pychron/dvc/func.py:171-240`) and reads (`pychron/dvc/util.py:72-106`,
which requires `preferred` and `sample_metadata`).

File name: `<identifier[:3]>/ia/<identifier[3:]>.ia.json` in early 2018,
later `<identifier[:3]>/ia/<identifier[3:]>_<NNNNN>.ia.json` with a
5-digit counter from `00000` (`dvc.py:2162-2181`). Both names occur in one
repo (NMGRLData/IR998: `657/ia/61.ia.json` and `657/ia/61_00000.ia.json`,
path listing only). An identifier can have several interpreted ages.

## 6. Meta repo layout

Read by `pychron/dvc/meta_repo.py` and `pychron/dvc/meta_object.py`.

| What | Path | Fixture | Source |
|---|---|---|---|
| level (flux positions) | `<irradiation>/<level>.json` | `meta/NM-293/G.json` | `meta_repo.py:353-354` |
| level to production name | `<irradiation>/productions.json` | `meta/NM-293/productions.json` | `meta_repo.py:315-337`, `:761-787` |
| production | `<irradiation>/productions/<name>.json` | `meta/NM-293/productions/Triga_PR.json` | `meta_repo.py:261-263` |
| chronology | `<irradiation>/chronology.txt` | `meta/NM-293/chronology.txt` | `meta_repo.py:63-65` |
| gains | `spectrometers/<name>.gain.json` | `meta/spectrometers/jan.gain.json` | `meta_repo.py:85-91` |
| sensitivity | `spectrometers/<name>.sens.json` | `meta/spectrometers/felix.sens.json` | `meta_repo.py:714-742` |
| irradiation holder | `irradiation_holders/<name>.txt` | `meta/irradiation_holders/24_hole.txt` | `meta_repo.py:53-55` |
| load holder | `load_holders/<name>.txt` | `meta/load_holders/37-hole.txt` | `meta_repo.py:440-452`, `:809-813` |

Other things in the repo at the pinned commit, not copied: top-level
`productions/*.json` (19 files; not read by `get_production`, which uses the
per-irradiation directory), `scripts/<spectrometer>/`,
`experiments/<spectrometer>/` (`meta_repo.py:216-222`),
`molecular_weights.json`, `reactors.json`, `correlation_ellipses.json`,
`data_reduction_log.json`, `dr_manifest.json`, `README.md`,
`irradiation_holders/*.xml`, and a file literally named
`irradiation_holders/.txt`. The source also names `sensitivity.json` and
`cocktail.json` at the root (`meta_repo.py:815-817`, `:617-638`); neither
exists at the pinned commit.

### 6.1 Level file

`meta/NM-293/G.json` is `{"positions": [...], "z": 0}` with 23 positions.
An older form is a bare list of positions (`meta_repo.py:820-826`,
**source only**). Per position (`meta_repo.py:566-585`):

| Key | Example (identifier `66052`) |
|---|---|
| `position` | `16` (integer hole number) |
| `identifier` | `"66052"` |
| `j`, `j_err` | `0.0018848683037985877`, `6.292765550352345e-07` |
| `mean_j`, `mean_j_err` | `0.0`, `0.0` |
| `decay_constants` | `{"lambda_k_total": 5.464e-10, "lambda_k_total_error": 0}` |
| `options` | `model_kind`, `monitor_sample_name`, `monte_carlo_ntrials`, `predicted_j_error_type`, `use_monte_carlo`, `use_weighted_fit` |
| `analyses` | `[]` here; for monitor positions a list of `{"record_id", "uuid", "status"}` |

Keys in the source and not in the fixture: `mean_j_mswd`, `position_jerr`,
`monitor` (`name`, `age`, `error`, `material`; `meta_repo.py:692-698`), and
`is_omitted` in place of `status` in `analyses` (`meta_repo.py:577-584`).

History of this file (11 commits), newest first: `<RECOVER> MetaData`
(2025-10-17), `repo updated for analysis ...` and its `Revert "..."` (2021,
2024, 2025), `modified - G.json` (2018-06-05), three `Labnumber Entry Save`,
`Added level G to NM-293` (2017-12-20). Flux fits elsewhere use
`fit flux for <irradiation><level>` (`meta_repo.py:650-653`; 1040 such
commits in the repo). The commit made after every analysis is
`repo updated for analysis <runid>` (`dvc_persister.py:433-435`; 4479
commits).

### 6.2 Productions, chronology, gains, sensitivity, holders

- `productions.json`: `{"<level>": "<production name>"}`; may also carry a
  `note` key (`meta_repo.py:318-319`).
- Production: `{"<ratio>": [value, error]}` for `Ca3637`, `Ca3837`,
  `Ca3937`, `Ca_K`, `Cl3638`, `Cl_K`, `K3739`, `K3839`, `K4039`. Optional
  string keys `reactor` and `name` (`meta_object.py:202-217`).
- Chronology: text, one dose per line, `power,start,end` with
  `%Y-%m-%d %H:%M:%S` **naive local** times (`meta_object.py:79-94`,
  `pychron_constants.py:434`). Fixture:
  `1.0,2017-12-21 06:28:00,2017-12-21 14:28:00`. Lines that do not split
  into three are skipped.
- Gains: a JSON object detector to gain (`meta_object.py:50-58`). `{}` in
  the fixture. Real per-analysis gains are in the project repo's
  `<spec_sha>.json` and in `detectors.<det>.gain`.
- Sensitivity: a JSON **list**, order significant; the last entry is the one
  the legacy code uses (`meta_repo.py:744-753`). Per entry `create_date`
  (`%Y-%m-%d %H:%M:%S`, naive), `mass_spectrometer` (lower case `"felix"`),
  `sensitivity`, `units` (`"mol/fA"`), `note`, `user`. The fixture's four
  entries are **not in date order** (2020, 2000, 2020, 2021) and two are
  identical.
- Holder (irradiation and load share one parser, `meta_object.py:260-299`):
  first line `<x>,<radius>[,<has hole number>]`, where `<x>` is ignored
  (`56` in `24_hole.txt`, `circle` in `37-hole.txt`); then one hole per
  line as `x,y` or `x,y,r` (or `id,x,y` when the third header field is
  true). Blank lines and lines starting with `#` are skipped but still
  advance the hole number (`enumerate`, line 276).

## 7. Legacy MySQL catalog (`pychron/dvc/dvc_orm.py`)

Table names equal the class names (`__tablename__` returns `__name__`,
lines 47-50). `id` is an integer primary key (line 54). Column names are
case-sensitive as written. No real dump was available: this section is
**source only**, and column types in a real database may differ.

| Catalog entity | Table | Columns | Lines |
|---|---|---|---|
| principal investigator | `PrincipalInvestigatorTbl` | `id`, `affiliation`, `email`, `last_name`, `first_initial` | 460-464 |
| project | `ProjectTbl` | `id`, `name`, `principal_investigatorID` -> `PrincipalInvestigatorTbl.id`, `checkin_date`, `comment`, `lab_contact`, `institution` | 329-339 |
| material | `MaterialTbl` | `id`, `name`, `grainsize` | 354-356 |
| sample | `SampleTbl` | `id`, `name`, `materialID` -> `MaterialTbl.id`, `projectID` -> `ProjectTbl.id`, `note`, `igsn`, `lat`, `lon`, `storage_location`, `lithology`, `unit`, `lithology_class`, `lithology_type`, `lithology_group`, `location`, `approximate_age`, `elevation`, `create_date`, `update_date` | 368-387 |
| irradiation | `IrradiationTbl` | `id`, `name`, `create_date` | 422-424 |
| level | `LevelTbl` | `id`, `name`, `irradiationID` -> `IrradiationTbl.id`, `holder`, `z`, `note` | 397-405 |
| position and identifier (labnumber) | `IrradiationPositionTbl` | `id`, `identifier`, `sampleID` -> `SampleTbl.id`, `levelID` -> `LevelTbl.id`, `position`, `note`, `weight`, `j`, `j_err`, `packet` | 427-436 |
| user | `UserTbl` | `name` (primary key), `affiliation`, `category`, `email` | 488-492 |
| mass spectrometer | `MassSpectrometerTbl` | `name` (primary key), `kind` | 449-451 |
| extract device | `ExtractDeviceTbl` | `name` (primary key) | 456-457 |
| load | `LoadTbl` | `name` (primary key), `create_date`, `archived`, `username` -> `UserTbl.name`, `holderName` | 497-503 |
| load position | `LoadPositionTbl` | `id`, `identifier` -> `IrradiationPositionTbl.identifier`, `position`, `loadName` -> `LoadTbl.name`, `weight`, `note`, `nxtals` | 508-514 |

The identifier (labnumber) is not its own table: it is the `identifier`
column of `IrradiationPositionTbl`.

Related tables the importer may need:

| Table | Columns | Lines |
|---|---|---|
| `AnalysisTbl` | `id`, `experiment_type`, `timestamp` (DATETIME, naive local: `datetime.now()`, `dvc_persister.py:309-312`, `:591`), `uuid`, `analysis_type`, `aliquot`, `increment`, `irradiation_positionID` -> `IrradiationPositionTbl.id`, `measurementName`, `extractionName`, `postEqName`, `postMeasName`, `mass_spectrometer` -> `MassSpectrometerTbl.name`, `extract_device`, `extract_value`, `extract_units`, `cleanup`, `duration`, `weight`, `comment`, `pre_cleanup`, `post_cleanup`, `cryo_temperature` | 104-159 |
| `AnalysisChangeTbl` | `idanalysischangeTbl`, `tag`, `timestamp`, `user`, `analysisID` -> `AnalysisTbl.id`. **The only store of an analysis tag when git tagging is off** (section 4.3). | 95-101 |
| `RepositoryTbl` | `name` (primary key), `principal_investigatorID` | 64-68 |
| `RepositoryAssociationTbl` | `idrepositoryassociationTbl`, `repository` -> `RepositoryTbl.name`, `analysisID` | 87-90 |
| `MeasuredPositionTbl` | `id`, `position`, `x`, `y`, `z`, `is_degas`, `analysisID`, `loadName` -> `LoadTbl.name` | 517-525 |
| `SamplePrepWorkerTbl`, `SamplePrepSessionTbl`, `SamplePrepStepTbl`, `SamplePrepImageTbl`, `SamplePrepChoicesTbl` | see source | 533-586 |
| `IRTbl`, `AnalysisGroupTbl`, `AnalysisGroupSetTbl`, `MediaTbl`, `CurrentTbl`, `ParameterTbl`, `UnitsTbl`, `VersionTbl`, `RestrictedNameTbl` | see source | 528-655 |

`AnalysisTbl.uuid` is declared `String(32)` (line 108) although a uuid with
dashes is 36 characters. **Unconfirmed** what a real database stores.

## 8. Findings that differ from the ingestion spec or the plan

1. Raw data suffix is `.dat.json`, not `.data.json` (section 3.2).
2. A path gives a runid in older repos and a **uuid** in newer ones; the
   prefix length is 2, 3, 4 or 5 (section 3.1). The spec's
   "path -> (runid, kind)" must be "path -> (runid or uuid, kind)".
3. Real repos contain file types the spec's kind list does not name:
   spectrometer settings (`<40 hex>.json`, one or more per repo, added inside
   `<COLLECTION>` commits), run logs (`.logs.log`), root-level frozen
   productions. Under spec section 4.4 "a file of unknown type becomes an
   `unparseable` conflict", every repo would report conflicts for them.
4. Interpreted-age files exist in two formats; the nested one has no
   `identifier` key (section 5.9). Per-analysis `age` and `age_err` are
   present in both, as the spec assumes.
5. Ages stored in an interpreted-age file were computed from the files as
   of the `<IA>` commit. Later commits change those files (IR1010 `5283d6c`),
   so age parity against the *imported heads* fails for this fixture by
   construction (section 1.1).
6. Tags are in git only for older data. With `USE_GIT_TAGGING = False` they
   exist only in MySQL `AnalysisChangeTbl`, which the spec's catalog list
   (section 4.2 of the spec) does not include.
7. The blank analysis is in a different repo from the unknown. A project
   repo's blank and IC-factor `references` name analyses (by `record_id`,
   and `uuid` once reviewed) that live in other repos.
8. `icfactors` `references` can be the string `""`; blank `exclude` can be a
   boolean or a string (section 5.5).
