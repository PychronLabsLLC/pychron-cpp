# EarthBank (`libs/earthbank`, `libs/earthbank_qt`): agent rules

Part of the agent guide: root `AGENTS.md` applies in full, this adds what is
only this component's. Kept compressed, as that file is.

- EarthBank (AusGeochem) publishing: `libs/earthbank` Qt-free, no network
  code. Network behind `IHttpClient` (`http.hpp`); `libs/earthbank_qt` is the
  HTTPS adapter, built only when Qt6 Network is found (elsewhere
  `make_http_client()` is null and programs say so). One `Submission` per
  sample (`mapping.hpp`) feeds pre-flight, workbooks and upload: no route
  builds its own. Record field names are the service's verbatim, misspellings
  included (`apparentAgeUncertainityTypeName`, `stratographicUnitName`).
  Rules easy to break:
  - No test, example, script or default config reaches EarthBank: production,
    no sandbox, a record cannot be taken back. Tests talk to `FakeEarthBank`
    (`tests/earthbank`); what it assumes of the service is numbered in
    `tests/earthbank/fixtures/README.md`. Live test
    (`pychron_earthbank_live_tests`, label `live`; `ctest -LE live` leaves it
    out) is skipped unless a person sets the four
    `PYCHRON_TEST_EARTHBANK_*` variables; never set in CI. Its steps
    (`tests/earthbank/live_round_trip.hpp`) run against the fake on every
    build. Production host is named once, in `profiles.hpp`.
  - No password or token in a file, a log line, an error or any output. A
    request is logged as method, path, status. What the service said reaches
    the user only through `Session::failure`, which strikes both first.
    `profiles.toml` never holds a password (`ISecretStore`).
  - Uploader journals before and after each write and never guesses the
    outcome of an unanswered one: a write with no answer, or a 5xx fault, is
    in doubt, and nothing more of that sample is sent until the user says
    whether the record is there. A request is sent at most once: no retry
    in a client, no redirect followed.
  - A flux monitor is never resolved from a citation alone, and a wrong
    rewrite into a list's spelling is worse than a miss: a case added to
    `vocab.cpp` gets its test.
  - Templates in `libs/earthbank/templates` are the vendor's files, shipped
    as they are: never edited, re-saved or regenerated.
  - UI: everything that asks the service or the password store runs on
    `EarthbankWorker`'s thread (real time, not a clock participant), one
    request at a time; an upload sends what its confirmation showed, with the
    session and lookups of that confirmation. EarthBank page and Publish
    dialog read nothing (profiles file, keychain) before they are shown.
  User guide and live-test runbook: `docs/earthbank.md`.
- `libs/earthbank_qt` (`QtHttpClient`, the HTTPS side of `libs/earthbank`)
  builds only when Qt6 Core and Network are found (`-DPYCHRON_EARTHBANK_NETWORK=OFF`
  skips it); `libs/earthbank` has no network code and goes through `IHttpClient`.
  `QtHttpClient` is an HTTP/1.1 client over `QSslSocket`, not
  `QNetworkAccessManager`, because the manager re-sends a request whose connection
  closes, which the seam forbids. Without it `make_http_client()` is null
  (`apps/elctl/src/earthbank_no_network.cpp`) and `elctl earthbank login|account|
  packages|preflight` say so and exit 2. Tests talk to a `QTcpServer` on 127.0.0.1
  or to `FakeEarthBank`, never to the service.
