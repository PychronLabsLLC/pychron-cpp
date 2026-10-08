# EarthBank fixtures

What `FakeEarthBank` (`../fake_earthbank.hpp`) starts from: the service's
answers, as files. No test talks to EarthBank; these stand in for it.

**Everything here was written by hand. No file is a capture of the service.**
The shapes come from the legacy integration (the University of Melbourne fork
of pychron, `pychron/ausgeochem`: its client, its README and its probe
scripts) and the names of the controlled terms from the vendor's two template
workbooks (`libs/earthbank/templates/`).

**Every person, login, institution, package and id is invented.** The account
is nobody's, the institutions do not exist, the password in the fake's options
opens nothing, and no id here is an id of the real service: not a package's,
not a person's, not a term's. A change to these files keeps it so. The names
of the terms themselves (minerals, flux monitors, methods, archives) are the
vendor's published lists and are real.

| File | What it is |
|---|---|
| `account.json` | the answer of `GET /api/account` for the one account; its `login` is the user name |
| `packages.json` | the rows of `GET /api/management/data-packages`, before a test adds any |
| `permissions.json` | not an answer of the service: what the fake must know and no answer says. `createdById` is who the account is as the maker of a package (it is not the account's `id`), `writablePackageIds` the packages of `packages.json` it may write to |
| `lookups/<name>.json` | the rows of one vocabulary, `<name>` being the last part of its endpoint (`/api/arar/FluxMonitor`, `/api/core/l-sample-kinds`) |

The lookups:

- The Ar-Ar lists (`DecayConstant`, `AirRatio`, `FluxMonitor`, `LArArMethod`,
  `LAnalysisScale`, `LUncertaintyUnit`, `LAgeType`,
  `LArArDataInterpretationTool`, `LArArInterpretation`) are the columns of the
  Ar-Ar template's `Lookup Tables` sheet, whole and in its order, entry for
  entry as it spells them: the space that ends `Renne et al. 2011 ` and three
  of the interpretations is the template's. A test
  (`FakeEarthBank.TheArArListsAreTheTemplates`) holds the files to the sheet,
  so the workbook route and the REST route know the same terms.
- `l-sample-kinds`, `l-sample-methods`, `l-location-kinds` and `archives` are
  columns of the Sample template's `Lookup tables` sheet (`Unknown` is in its
  archive list twice, and so here).
- `materials` is 72 of the ten thousand entries of that sheet's `Lithology`
  column: the minerals and rocks an argon lab dates, every entry beginning
  `Biotite` (more than a page of them, for paging), and a few with characters
  that need care in a URL or in JSON (`&`, `+`, quotes, an apostrophe, and
  `Chabournéite Group`, respelled: the template's copy of it is mis-encoded).
- `l-error-types` is the trap legacy warns of: the list that looks like the
  uncertainty types, whose ids a record refuses. Its names are the template's
  `Uncertainty Type` column.
- `stratigraphic-units`, `fundings` and `literature` have no published list;
  their few entries are invented.

Ids are in a range of their own per list, and none is 1 or 2 (the two ids of
the uncertainty types), so an id of the wrong list is never by chance a right
one.

## Replacing these with captures

For someone with an EarthBank login, by hand; nothing of it runs in a test.
**Every step here only reads.** On the legacy fork, with `EARTHBANK_USER` and
`EARTHBANK_PASS` in its gitignored `pychron/ausgeochem/.env`:

```
EARTHBANK_KNOWN_PACKAGES=<an id of a package of yours> \
python -m pychron.ausgeochem.tests.packages_probe             # /api/account, that package, the package list
python -m pychron.ausgeochem.tests.integration_test --smoke   # no login: which lookups answer
```

- `packages_probe` prints the account, one package in full and the first row
  of the list: these replace `account.json` and the rows of `packages.json`.
  Set `EARTHBANK_KNOWN_PACKAGES`: without it the script asks for two package
  ids that are hard-coded in it.
- `integration_test --smoke` prints how many entries each lookup gave to a
  caller with no login, not the entries. It settles which lists need one
  (assumption 11 below).
- For the rows themselves, save a plain `GET <endpoint>?size=2000` of each
  list into `lookups/`, with a login for the lists that need one.

**Do not run** `arar_post_probe`, `integration_test` without `--smoke`, or
`live_upload_test`. Each of them writes to the production database, which has
no sandbox and takes nothing back, and leaves a record there.

The bodies of errors (the 500 of a package that is not writable, a constraint
failure) are not captured here. They are written down during the by-hand
live run, which works in a throwaway package (`docs/earthbank.md`, the plan's
Task 14).

Before a capture is committed, every real name, login, e-mail address,
institution, package name and id in it is replaced by an invented one (the
ids of terms in `lookups/` too, if the tests are to keep their ranges), and
`permissions.json` is brought in line. Where a capture differs from what the
fake does, the fake is what changes: correct `fake_earthbank.cpp`, strike the
assumption below, and let the tests that then fail say what relied on it.

## Assumptions to check against the real service

What legacy records, the fake reproduces, and a comment in
`fake_earthbank.cpp` says where it was learned. What legacy does not record
was chosen as plainly as could be and is marked `// assumed (n):` there, `n`
being its number here; a test (`FakeEarthBank.EveryAssumptionIsMarkedAndListed`)
holds the marks and this list to each other. Until someone has checked an item
it is a guess, and code that passes against the fake may still meet something
else. Where a client has to be right whichever way the service turns out, the
fake has a control for the other way, named with the item.

### Every answer

1. A list is cut into pages of 20 unless `size` says otherwise, of at most
   2000; `page` counts from 0. (Legacy asks for `size=2000` and takes what
   comes as the whole list.)
2. A login answers 200 and everything created answers 201. (Legacy takes any
   2xx. `created_status(200)` gives the other.)
3. An error legacy does not quote has JHipster's body: `type`, `title`,
   `status`, `path`, `message`, and `detail` when there is one.
4. A body sent without `Content-Type: application/json` is 415; a body that
   is not a JSON object is 400. (Legacy sets the header on every POST.)
5. A path the service has, asked with a method it has not, is 405. A path it
   has not is 404 (legacy: 404 is the wrong address).
6. A request target holding a character that had to be encoded is 400, and a
   `+` in a query is read as a space.
7. An id sent as a string of digits is taken as the number.

### Login and account

8. Wrong credentials are 401 with `detail` "Bad credentials"; a request that
   needs a login and has none, or a token no longer good, is 401 with
   `detail` "Full authentication is required to access this resource".
   (Legacy acts on the status alone.)
9. The fields of `account.json` beyond `id` (JHipster's user: `login`,
   `firstName`, `lastName`, `email`, `activated`, `langKey`, `authorities`).
10. The token is treated as opaque. The real one is a JWT.

### Lookups

11. The lists that need a login are those at `/api/core/l-*` and no others:
    `/api/core/materials`, `archives`, `stratigraphic-units`, `fundings` and
    `literature` answer without one, as everything under `/api/arar` does.
    (Legacy says only "ArAr lookups are public, but /api/core/L* lookups need
    auth".) A token that is no good is treated as none, so the open lists
    stay open to it.
12. A row has `id` and `name` and nothing else. (Legacy also reads
    `reference` and `description` where a row has them; whether any does is
    not known. A test adds such a row with `add_lookup_row`.)
13. The names the REST lists hold are the names of the template's lists, to
    the letter, the stray spaces included, and in particular `/api/arar/LAgeType`
    holds `Plateau Age`, `Weighted Mean Age`, `Inverse Isochron Age`,
    `Total Gas Age`.
14. `name.contains` does not mind the case (`name.equals` does: legacy). A
    parameter a list does not know is ignored. Without `sort` the rows come
    in the order the service keeps them, here the file's.
15. `/api/core/l-error-types` holds the template's `Uncertainty Type` names.
    (Legacy: it holds a `1 sigma` whose id a data point refuses.)

### Packages

16. Every package is listed to every account; `name.equals` filters as
    `name.contains` and `institutionId.equals` do.
17. A row is `dataPackageDTO` and the counts (`countEditors`,
    `countSupervisors`, `countSamples`, `countArArDataPoints`), with no copy
    of `id` or `name` beside them, and a field that is not set is `null`.
    (Legacy reads `dataPackageDTO` first and a copy on the row second.)
18. A package made from a flat body loses every field, not the name alone,
    and is called `Unnamed data package`. (Legacy: the name is ignored and
    the package gets a default one.) The answer is the row as it is listed.
    Two packages may have one name.
19. `GET /api/management/data-packages/<id>` of an id that is not there is
    404. `DELETE` of it answers 204, takes with it what is in the package,
    and is refused like any other write when the package is somebody else's.

### Writes

20. "Package is not writable by user." is the `title` of the 500 and the
    whole of its body: `{"title":"Package is not writable by user."}`.
    (Legacy quotes the status and the sentence, and reads `detail`,
    `message` or `title`.)
21. A `*Id` that is no row of its list is 400, and its `detail` is Spring's
    sentence with PostgreSQL's inside: `could not execute statement; SQL
    [n/a]; constraint [fk_<table>_<column>]; nested exception is ...: ERROR:
    insert or update on table "<table>" violates foreign key constraint
    "fk_<table>_<column>"`, then `Detail: Key (<column>)=(<id>) is not present
    in table "<list>".` (Legacy reads the column out of `Key (...)` or out of
    `constraint "fk_..."` and cuts the text at `;` and `nested exception`;
    the status, the tables' names and the rest of the wording are guessed.
    Only `ar_ar_data_point` and `l_uncertainty` are names legacy gives.
    Legacy looks for the word "constraint" before it looks at the status, so
    the status may as well be 500: `constraint_status(500)`.)
22. The only ids of an uncertainty type are 1 and 2. (Legacy knows these two;
    the table has more rows.)
23. A term's `*Name` with no `*Id` beside it is 400. (Legacy drops such a
    name "so the server doesn't 400 on the row"; the body is guessed.) A name
    beside its id is ignored. A name with no list (`plateauStepName` and the
    other two steps, the uncertainty types' names, `lithologyName`) is taken
    as it comes. A field the record does not have is kept and does no harm.
24. A sample needs its package on the wrapper and in `sampleDTO`, the same in
    both, or it is "not writable". (Legacy stamps both because it does not
    know which is read; the fake takes only what legacy is known to have
    sent.)
25. The answer to a new sample is the body as sent with the ids filled in:
    `sampleDTO.id`, `sampleDTO.locationId`, `locationDTO.id`, and no `id` on
    the wrapper. (Legacy reads `sampleDTO.id`, then `sample.id`;
    `sample_id_place` puts the id there, or on the wrapper.) An empty `locationDTO` makes
    no location. A second sample of the same name in a package is a second
    sample. The search needs a login.
26. The answer to a new data point is the body as sent with `id` on top (the
    `arArDataPointId`: legacy), the same in `extendingDataPointDTO.id` and
    `dataPointDTO.arArDataPointId`, and an id of its own in `dataPointDTO.id`.
27. A data point whose `sampleId` is no sample is a constraint error; one
    with none is accepted; a `dataStructure` other than `ARARDATAPOINT`, or no
    `extendingDataPointDTO`, is 400. (That a sample of *another package* is
    accepted is legacy's finding, not a guess.)
28. `arArSetUpName` and `arArIrradiationName` beside the two records are kept
    and not checked. (Legacy's client could send them; its upload never did,
    so the service has never been seen to take them.)
29. An aliquot, measurement, age calculation or age summary whose
    `arArDataPointId` is missing or is no data point is "not writable", as a
    data point without its umbrella is. A measurement's `aliquotName` need
    not be the name of an aliquot. Posting the same record twice makes two.

### Further

30. The sample search keeps to `dataPackageId.equals`. (Legacy does not
    trust it to and checks each row's `sampleDTO.dataPackageId` itself;
    `ignore_sample_package_filter(true)` is the service that does not.)
31. A second data point of the same name for the same sample is a second
    data point: nothing is refused as a copy.
32. No field of any record is required and none but an id is held to a type:
    a data point with no date, a measurement with text for an age, are taken
    and kept as sent.
33. `sort=<field>,ASC|DESC` works on a lookup as on the package list. (Legacy
    sends it for packages only.)
34. Nothing in a sample's `locationDTO` is checked against a list.
35. A package is deleted at `/api/management/data-packages/<id>`. (Legacy's
    README gives DELETE for the list's path and says no more.)
