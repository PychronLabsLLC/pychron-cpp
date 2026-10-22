# Publishing to EarthBank

How to get 40Ar/39Ar results out of pychron and into EarthBank (AuScope's
AusGeochem data platform), from `pychron-ui` or `elctl earthbank`: by
uploading them, or by writing EarthBank's own import workbooks. The code is
`libs/earthbank`; `elctl earthbank help` and the
[elctl reference](user/08-elctl-reference.md#earthbank-workbooks) have every
flag. The last part of this page is for whoever maintains the code.

Pychron cannot change or remove a record on EarthBank. Everything below
follows from that: it shows what it will send before it sends it, it never
sends a record twice, and when it cannot tell whether a record arrived it
stops and asks you.

## What you need

- **An EarthBank account** (a user name and a password). EarthBank's support
  gives them out.
- **A data package you may write to.** Everything on EarthBank (samples,
  data points, ages) is kept in a data package. Your account must be an
  editor or supervisor of the one you upload to. A package you make from
  pychron is yours, so it can always be written to.
- **Your institution's id on EarthBank**, a number. It is used to list your
  institution's packages and to make new ones under it. Ask your EarthBank
  administrator if you do not know it.
- **Your lab's analytical set-up, by the name it has on EarthBank.** Every
  data point refers to one, and it must already exist there.

To write the workbooks and import them on the website you need none of these
in pychron, only a login on the website.

## Set up once

A **profile** is a name for one account on one server: the server's address,
your login, your institution's id and the data package uploads go to. The
window and the command line share the profiles, so do this in either.

In the window:

1. **File > Preferences > EarthBank**. Press **Add** and fill in the row:
   a name of your choosing (letters, digits, `-`, `_`), your user name and
   your institution's id. The server is already EarthBank's.
2. Type your password in **Password of the selected profile** and press
   **Test**. The line under it says `signed in as <your name>`. A password
   that signs in is kept from then on, and the field is emptied.
3. Press **Select / Create Package...**. Pick a package from the list (the
   box above it filters by name), or type a name and press **Create**.
   **OK** puts its id in the row.
4. Tick **Active** on the profile to use when none is named, then **OK**.

On the command line:

```bash
elctl earthbank login --profile mylab --username a.user --institution 1001
elctl earthbank packages                       # the institution's packages
elctl earthbank packages --create "Ar-Ar results 2031"
```

The profiles are one file, written when you press OK or Apply:
`~/Library/Application Support/Pychron/earthbank/profiles.toml` on macOS,
`%APPDATA%\Pychron\earthbank\profiles.toml` on Windows,
`~/.config/pychron/earthbank/profiles.toml` on Linux. **A password is never
in it.** The operating system keeps it: the login keychain on macOS, the
Credential Manager on Windows, the secret service on Linux. The page says
which under the password field. Where there is none it says `Passwords are
not stored: this session only`, and you type it again next time. Removing a
profile and pressing OK removes its password too.

Profiles from legacy Pychron are brought over, without their passwords, with
`elctl earthbank import-profiles <its preferences file>`.

## Publish from the Publish dialog

EarthBank is a tab of the Publish dialog, which works on a release. Two ways in:

1. **View > Data**. Filter to the analyses you want, select them and press
   **Publish** on the toolbar. The dialog opens on a new release made of
   exactly those analyses. Publish acts on what is selected and nothing else.
   The button is off on an install without a database.
2. **View > Publish…**, select a release, press **Publish…**. The dialog opens
   on that release.

In the dialog tick **EarthBank** under Destinations; its tab holds the
account, the package, the action, the Analytical set-up and one row for each
sample. Choose the action and press **Run**:

- **Pre-flight** signs in and asks EarthBank whether it knows every term the
  upload would send. Nothing is written. Do this first.
- **Export xlsx** writes the workbooks into a folder. No account needed.
- **Upload** sends the records. It is offered only for a release that is
  published, public and not under embargo: EarthBank has no embargo or
  versioning, so a release is the citable unit and EarthBank one more place
  it is listed. When Store is ticked in the same run, the release is
  published first and the upload checked again after.

For a new release the per-sample table fills while the run is under way, from
what you chose before for each sample name, or from the rule's defaults. To
review the choices first, make the release (a run with no destination
ticked), then open the dialog on it.

### The rows

| Column | What it is |
|---|---|
| Sample, Analyses | The sample's name and how many of the selected analyses are its. |
| Interpretation | What the age dates, by EarthBank's name for it (`Eruption age`, ...). Empty is sent as `Unknown`. Pre-flight says if EarthBank does not know the name. |
| Preferred age | The one age reported for the sample. The first choice is the rule's, with the age it gives; under it is each kind pychron can compute for the sample, with its age and one-sigma uncertainty in Ma. A kind that cannot be computed is greyed, and hovering says why. |
| Flux monitor | The monitor found on the analyses. Type another to replace it. |
| Notes | After a pre-flight, the terms of this sample EarthBank does not know. Hover for what pychron noted while building the sample. |

**The rule for the age**: a sample with heating steps reports its plateau
when it has one and its total gas (integrated) age when it has not; a sample
of fusions reports the weighted mean. A sample with more than one
step-heated aliquot is not left to the rule: the row says **Choose an
age...** and the sample stays out until you do.

**A greyed row** is a sample that cannot be published as it stands: its
analyses disagree about what the sample is (two materials, two locations,
two flux monitors, different constants), or none has an age. Notes says which
two values and which runs. The other samples go on without it. Correct the
analyses or the catalog, then publish that sample.

What you set in a row is remembered on this computer by sample name, so the
next time the sample comes up it is as you left it. It is not kept on
EarthBank.

**Decay constant** and **Air ratio** are left empty to be named from the
values the reduction used; fill one in only to say it differently.

### Uploading

Pressing **Run** on an upload first signs in and asks EarthBank who you are
and what the package is, checks every term, and then shows:

- the account's name and login, and the data package's name and id, as
  EarthBank has them;
- for each sample, the records it will write: one data point, its aliquots,
  one measurement for each analysis, an age calculation and an age summary
  (and the sample itself, if the package has none of that name).

Nothing has been sent at that point. **Yes** sends it; **No** sends nothing.

Then one line appears for each record as EarthBank takes it, with the id it
gave, and at the end one line for each sample:

| It says | Meaning | What to do |
|---|---|---|
| `complete, data point <id>` | Everything is on EarthBank. | Nothing. |
| `stopped` | You pressed **Cancel run**, or something failed before a record was sent. Nothing is in doubt. | Run the upload again. Only what is missing is sent. |
| `partial` | The data point is there and some of its records are not: EarthBank refused one, or the connection failed. | Read the message, fix the cause, run again. |
| `in doubt` | A record was sent and no answer came back. It may or may not be there. Nothing more of that sample was sent. | See below. |
| `refused` | Not started. The message says why. | See below. |

**Cancel run** stops after the record that is being sent. Closing the dialog
during an upload does the same.

**In doubt.** A record sent twice is there twice and cannot be removed, so
pychron does not guess. Two buttons appear under the run line. Open the data
package on EarthBank's website and look for the record the message names:

- it is there: type its id and press **It is there, with this id**;
- it is not: press **It is not there**.

Either finishes the upload you already agreed to. When the record in doubt
is the sample itself there are no buttons: run the upload again and the
package is searched for a sample of that name. When two samples are in doubt
they are settled one at a time.

**Terms EarthBank does not know** stop an upload before the question, and
are written in the rows. Correct them (the interpretation, the monitor, the
constants' names), or tick **Allow unresolved terms** to send the records
without those fields. A term that *could not be asked about* always stops
it: EarthBank may know it, so try again.

**Refused, the data changed.** What was uploaded for this sample before is
not what you are uploading now (other analyses, another age, another
interpretation). EarthBank cannot be updated, so nothing is sent. Tick
**Send as new records** to send the sample again as new records; the old
ones stay on EarthBank until you remove them there.

What has been written is kept in a **journal** on this computer, one file
for each sample and data package, in the `journal` folder beside the
profiles file. It is the only record of an upload that did not finish. Do
not delete it, and finish an upload on the computer it was begun on.

## The same from the command line

```bash
# what would be sent, and whether EarthBank knows every term
elctl earthbank preflight --db sqlite:$HOME/pychron/data.db --sample MB-12

# shows the account, the package and the records, and stops
elctl earthbank upload --db sqlite:$HOME/pychron/data.db --sample MB-12 --overrides earthbank.toml

# the same, sent
elctl earthbank upload --db sqlite:$HOME/pychron/data.db --sample MB-12 --overrides earthbank.toml --yes
```

The overrides file says what the rows of the dialog say (interpretation,
preferred age, flux monitor, the set-up and the constants' names). When an
upload does not finish, the command prints the exact line that takes it
further. See the [elctl reference](user/08-elctl-reference.md#uploading).

## Workbooks instead of an upload

**Export xlsx** (or `elctl earthbank export-xlsx`) fills copies of the two
workbooks EarthBank hands out for importing, and you import them on
EarthBank's website yourself.

1. Choose **Export xlsx**, name a folder, and choose whether to write one
   workbook for all the samples or one for each, and whether to write the
   Sample workbook.
2. **Run**. The result lists the files and, under them, notes: a value that
   is not in one of the template's lists, a column the template marks
   Required that a row leaves empty. Read them before importing. **Show
   folder** opens the folder.
3. Sign in to EarthBank's website and import the workbooks there.

Nothing is ever overwritten: a second export into the same folder gets new
names.

## What is sent for a sample

Each sample becomes one data point with the one age reported for it.

| Record | Holds | From |
|---|---|---|
| Sample | name, material, elevation, IGSN, stratigraphic unit | the analyses and the catalog's sample row (Entry > Samples) |
| Location | latitude and longitude | the catalog's sample row |
| Data point | the date of the earliest analysis, the method (step heating or fusion, laser or furnace), mineral, lithology | the analyses and the catalog |
| Aliquot | one for each loaded piece, `<identifier>-<aliquot>`, with its weight in mg when known | the analyses |
| Measurement | one for each analysis: the five argon isotopes corrected and before correction, their blanks, the ratios to 39ArK, 40Ar*/39ArK, %40Ar*, the share of the 39ArK and its running sum for a heating step, Ca/K, K/Ca, Cl/K, J, the apparent age, the laser power or furnace temperature, the time, and whether the reported age used it | the reduction |
| Age calculation | the decay constant, the atmospheric 40Ar/36Ar and the flux monitor, by EarthBank's names | the reduction's constants and the position's monitor |
| Age summary | the reported age, its uncertainty, MSWD, probability, the number of analyses, the kind of age, the interpretation, and one sentence describing how the age was made | the reduction and your row |

Ages are in Ma whatever unit the lab displays. **Every uncertainty is one
sigma and absolute.** Intensities are in fA. The reported age's uncertainty
includes the J uncertainty, and the age summary's description says so; an
analysis's apparent age is analytical, with its J sent beside it.

The same values, with the same flags, are those of
`elctl export --group-by sample --sigma 1`: see [export](export.md).

**Left empty**, because pychron has nothing to fill it from: grain size,
K content, funding and literature, the kind of sample and how it was
collected, the analysis scale, comments, and the beam size (it is recorded
without a unit). An analysis without an age, or without all five argon
isotopes measured, is left out of its sample and counted in the sample's
notes. A number that is not finite is not sent and is named there.

**Names.** EarthBank keeps lists of what it knows: minerals, methods, decay
constants, air ratios, flux monitors, interpretations. Pychron sends a term
as the id of its row in the list, looked up by name. The decay constant and
air ratio are found from the values the reduction used; a flux monitor is
found by its name together with its age or its paper (`FC-2 28.201 Ma`
finds the Fish Canyon sanidine of Kuiper et al. 2008). A monitor is never
found by a citation alone, and a name that contradicts its own age is
refused: a wrong id in a published record is worse than a miss. A pre-flight
lists everything that was found as something other than what is written;
read that list, since an id that is written cannot be corrected.

## When something goes wrong

The message is one sentence. The common ones:

| It says | Meaning | What to do |
|---|---|---|
| `EarthBank did not accept the user name and password.` | Wrong login, or the password was changed. | Type the password again in Preferences and press Test (`elctl earthbank login`). |
| `EarthBank does not let this account do this` | The login is good; the account may not upload, or not there. | Ask EarthBank's support about upload permission. |
| `this account may not write to the data package` | You are not an editor or supervisor of the package. | Choose a package you made, or make one. |
| `it does not know the value sent for '<field>'` | EarthBank refused a record over a term that is not on its list. | Run a pre-flight, correct the term, run again. |
| `No data package is chosen` | The profile has none and none was named. | Select / Create Package. |
| `... is not known to EarthBank` | A term is not in EarthBank's list. | Correct it, or allow unresolved terms. |
| `EarthBank could not be reached` | Nothing left this computer. | Check the network and the server's address. Nothing was written. |
| `EarthBank did not answer` | The request was sent and no answer came. For a record: in doubt. | Follow "In doubt" above. Do not simply send again. |
| `EarthBank failed on its own side` | The service answered with a fault. For a record, it may or may not have been saved. | As above. |
| `the data changed since the upload of it began` | See "Refused, the data changed". | Send as new records, if that is what you mean. |
| `This build cannot reach EarthBank` | The program was built without network support. | Use a released build, or the workbooks. |

No message, log line or result ever holds your password or the session's
token.

---

# For maintainers

## How it is put together

`libs/earthbank` is Qt-free and opens no socket. One `Submission` per sample
(`mapping.hpp`) is what the pre-flight reads, what the workbooks are filled
from and what is uploaded. The network is behind `IHttpClient` (`http.hpp`);
`libs/earthbank_qt` is the one over HTTPS, built where Qt6 Network is found.
Passwords are behind `ISecretStore`. Record field names are EarthBank's,
verbatim, its misspellings included (`apparentAgeUncertainityTypeName`,
`stratographicUnitName`). The design, with what departs from legacy and why,
is `docs/superpowers/specs/2026-10-07-earthbank-design.md`.

EarthBank is in production, has no sandbox and cannot take a record back.
**No automated test, example or script contacts it.** Every test talks to
`FakeEarthBank` (`tests/earthbank/fake_earthbank.hpp`), which behaves as
legacy found the service to behave. Where legacy does not say, the plainest
thing was assumed and numbered; the assumptions are listed in
`tests/earthbank/fixtures/README.md` for someone with a login to check.

## The live test

`tests/earthbank_qt/test_live.cpp` uploads to a real EarthBank. It is
skipped unless a person sets all four variables, and no build or CI job
does:

```bash
export PYCHRON_TEST_EARTHBANK_URL=https://...       # the service
export PYCHRON_TEST_EARTHBANK_USER=...
export PYCHRON_TEST_EARTHBANK_PASSWORD=...
export PYCHRON_TEST_EARTHBANK_INSTITUTION=...        # the institution's id
cmake --build --preset dev --target pychron_earthbank_live_tests
build/dev/tests/earthbank_qt/pychron_earthbank_live_tests
```

Run it before a release that touches `libs/earthbank` or `libs/earthbank_qt`,
and after EarthBank announces a change to its API.

`ctest -LE live` leaves it out of a run altogether; `ctest -L live` runs
only it. In CI and in an ordinary `ctest --preset dev` it is listed and
skipped, saying which variables it wants.

What it does, in order:

1. Signs in and prints who the account is.
2. Makes a data package `pychron-test-<UTC date-time>` and **prints its name
   and id before anything else**. If the process dies after this, that is
   the package to remove by hand on the website.
3. Builds the two fixture samples (`tests/earthbank/analyses.hpp`: a
   step-heated sanidine and six biotite fusions) and runs the pre-flight.
   Terms the service does not know are printed and left out of what is sent;
   a term that could not be asked about fails the test.
4. Uploads both, printing each record's id, and requires each sample
   `complete` with every record acknowledged.
5. Uploads again and requires that nothing is sent.
6. Deletes the package, whether the steps before passed or not. A package
   that could not be deleted is a failure naming its id. The test sees that
   the package is gone, not what became of the samples and data points that
   were in it: after the first run, look on the website that they went with
   it, and remove them by hand if they did not.

The same steps run against the fake on every build
(`tests/earthbank/test_live.cpp`, `live_round_trip.hpp`).

What it does not do: read a data point back and compare values. No way to
read one is known. That comparison is step 5 of the first live run below, by
eye.

Two things it will tell you that nothing else can:

- **Step 3's printed misses and matches** are the first real check of
  `vocab.cpp` against the live lists. A name that differs is corrected
  there, with a case in `tests/earthbank/test_vocab.cpp`.
- **Step 6** rests on assumption 35 of the fixtures' README (where a package
  is deleted). If it fails, remove the package by hand and correct the
  assumption and the fake.

## The first live run

Not yet done. It is the owner's, with real credentials, and its outcome is
written here (date, what differed, what was fixed).

1. `elctl earthbank login`, then `elctl earthbank packages --create
   "pychron check A"` and note the id.
2. `elctl earthbank preflight` on one real sample. Correct `vocab.cpp` where
   a name differs and add the case to its test.
3. `elctl earthbank export-xlsx` of that sample, and import the workbooks
   through the website into package A.
4. `elctl earthbank packages --create "pychron check B"`, then
   `elctl earthbank upload ... --package <B> --yes` of the same sample.
5. Compare the two data points on the website, field by field. They were
   made from one `Submission` by two routes and should agree.
6. Delete both packages on the website.
7. Run the live test above.

Also still the owner's: reading the API licence linked from legacy's README
for any condition on clients (identification, rate limits). The client
identifies itself as `pychron-cpp/<version>`.

## Replacing the fake's fixtures with captures

The lists in `tests/earthbank/fixtures/` are hand-written with invented
people and ids. To replace one with what the service really returns:

1. Capture with a read-only request only. Legacy's `integration_test.py
   --smoke` prints counts and reads nothing else; legacy's
   `arar_post_probe.py` **writes to production** and is not a capture tool.
2. Remove every real person's name, login, institution member and package id
   from the capture before it is committed. No token, ever.
3. Run `ctest --preset dev -R 'earthbank|Earthbank'`. A test that fails is
   an assumption the capture has corrected: fix the fake and strike the
   assumption from the fixtures' README.

## A newer template from EarthBank

The two workbooks in `libs/earthbank/templates/` are EarthBank's files and
ship with pychron's installers. They are never edited, opened and saved, or
regenerated: the exporter copies one and fills rows, and everything else in
the copy is the template's, byte for byte. A newer template replaces a file
whole. Update its SHA-256 in `libs/earthbank/templates/README.md`, then run
`ctest --preset dev -R 'Xlsx|Ooxml'`: the exporter finds columns by the
field names in row 4 and lists by the template's own data validations, so a
template that keeps both needs no code. `PYCHRON_EARTHBANK_TEMPLATES` names
another folder of templates for a program that is already installed.
