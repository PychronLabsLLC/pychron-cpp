# Isotopx NGX driver: findings and open questions (not yet a spec)

Date: 2026-10-01
Status: Notes for a future spec
Owner: Jake Ross

The NGX driver was split out of the vendor-driver work because its hard problem
is not the wire format but sharing one TCP connection between three users:
the unsolicited acquisition event stream, magnet/source commands, and
extraction-line valve actuation. Legacy pychron spent years on this. These
notes record what a read of pychron's code and history showed, so the NGX spec
starts from it. Nothing here was run against an instrument.

## 1. What to trust in legacy pychron

- **Last lab-run state:** the 2026.3.0 release (back-merge `37b2ef166`,
  2026-03-21, with lab co-authors including `asu_ngx`). Its NGX behaviour is
  the September-October 2024 code: no device lock in the read path, valves
  wrapped in `SAB 1` / `SAB 0`, `triggered` flag, `acq_count` skip of the
  terminal `ACQ`, 0.25 s sleep after `StopAcq`.
- **Untested (owner's statement: Claude-generated NGX commits are mostly
  untested):**
  - `9b4d4cb58` (2026-05-15): device lock re-enabled around the whole
    `read_intensities`.
  - `9dd27fc97` (2026-06-11): `LineDemultiplexer` (opt-in, `use_demux`, off by
    default and enabled nowhere), re-login on reconnect, bounded valve retry
    (`max_valve_retries = 3`), partial-line stash.
- **Unknown provenance:** `93ec711d7` (2026-03-22, the day after the release,
  no Claude trailer): stale-event detection, scaled deadlines, parse helpers.
  Ask the owner.
- The C++ NGX codec (`libs/codecs/.../isotopx_ngx.hpp`) was written against
  pychron HEAD, so it inherits these doubts: default send terminator
  (`#\r\n` in the codec, `\r` in pychron), the demultiplexer, the error-code
  table.

## 2. History in one paragraph

2018: one `StartAcq` per read; `GetValveStatus` sometimes answers `E00` (the
previous ack). 2020: a device lock held across the integration starved valve
commands ("locking ... detrimental when doing long integration times").
2021: `triggered` flag to stop duplicate `StartAcq` (`E43`); valves send
`StopAcq` + `sleep(1)` before actuating, killing the integration; valve retry
when the reply is not `E00`/`OPEN`/`CLOSED`. 2023: read-size churn (8192, 2,
1 byte, select), `readline("#\r\n")` wins; `acq_count` to avoid double
counting `ACQ` + `ACQ.B`. August 2024: about 18 commits in one afternoon
trying reentrant locks and release counting; counts never balanced across
cancel/timeout/trigger paths. September 2024: all locks in `ngx.py` commented
out; valves use `SAB 1`, actuate, `SAB 0`. 2026: see section 1.

## 3. Suspected defect at pychron HEAD (static analysis only)

`read_intensities` holds `NGXController.lock` (a non-reentrant
`threading.Lock`); `_read_intensities` -> `trigger_acq` -> `self.ask`
resolves to `SpectrometerDevice.ask`, which takes the same lock. It would fire
whenever `read_intensities` is entered with `triggered == False`: the first
scan-window tick, peak center, or a run where another thread's `StopAcq`
clears the flag between trigger and read. Automated runs usually pre-trigger
outside the lock, which would hide it. Not executed; confirm before relying
on it.

## 4. Failure modes the legacy code fought

1. An event line read as a command reply (valves especially), and a command
   reply swallowed by the acquisition reader. Mitigated only by retry.
2. `GetValveStatus` answered with `E00`; unparseable replies read as "closed".
3. Two threads reading the socket (scan timer and collector): garbled records.
4. Duplicate `StartAcq` -> `E43`; `triggered` is an unsynchronised
   check-then-act.
5. A lock held across a long integration starves valves, magnet and source.
6. Lock acquire/release imbalance across trigger, cancel and timeout paths.
7. Valve actuation aborting an integration (pre-SAB).
8. Double counting the terminal `ACQ` and `ACQ.B`.
9. Leftover events after `StopAcq` or a magnet move passing as fresh data;
   nothing drains them, only a 0.25 s sleep.
10. A line split by a socket timeout is lost.
11. A silent reconnect leaves an unauthenticated session.
12. `GETMASS` and an integration-time change each kill the current
    integration with `StopAcq`.
13. Cancel does not interrupt a blocked read; one loop waits for integrated
    data with no deadline.

## 5. Invariants for a correct implementation

1. **One reader.** Exactly one place reads the socket, frames on `#\r\n`, and
   routes `#EVENT:` lines to an event channel and everything else to a reply
   channel.
2. **One in-flight command.** Send and wait-for-reply is atomic against other
   commands, including `StopAcq` and `SAB`. A late reply is discarded, never
   handed to the next command.
3. **The command lock is short and separate from acquisition.** Never held
   across an integration, so valves, magnet and source work during a 20 s
   integration.
4. **Nothing re-enters a non-reentrant lock.**
5. **Acquisition is one synchronised state machine** (idle, armed,
   integrating, complete, aborted); arming is an atomic transition, so no
   second `StartAcq` while one is active.
6. **Acquisitions carry a generation.** `StopAcq`, optional `SetMass` or
   period change, `StartAcq` is one atomic sequence; events from an older
   generation are dropped by generation, not by sleep or timestamp.
7. **Magnet move:** `StopAcq`, ack, `SetMass v,settle[,deflect]`, ack, settle,
   then `StartAcq`. An integration in flight is reported as aborted, not as a
   timeout or empty read.
8. **Integration change:** `StopAcq`, `SetAcqPeriod 1000`, reset, then
   `StartAcq N` on the next arm.
9. **Readbacks (`GETMASS`) need an explicit policy:** forbidden while
   integrating, or an explicit abort.
10. **Valve actuation is one unit:** `SAB 1`, `OpenValve`/`CloseValve`
    (expect `E00`), optional `GetValveStatus`, `SAB 0`; `SAB 0` on every exit
    path; concurrent actuations serialised.
11. **Valve actuation never stops, re-arms or discards an integration.**
12. **Valve state is three-valued:** open, closed, unknown. Garbage or a
    timeout is never "closed". Status polling uses the same serialised path.
13. **Retries are bounded and distinguish re-reading a reply from re-sending
    a command.** `StartAcq` is never blindly re-sent.
14. **Completion rule:** N per-second `ACQ` events (ATONA/CDD channels only
    when present) then one `ACQ.B`; the count advances exactly once.
15. **Timestamps:** decide between instrument time and host time explicitly;
    handle the instrument-to-host offset and midnight rollover; time zero is
    captured on the same basis.
16. **Cancel and timeout are prompt and leave idle,** with `StopAcq` sent and
    acknowledged and the event channel flushed.
17. **Reconnect restores the session (Login) before any queued command,**
    and invalidates the current generation.
18. **Aborts caused by a deliberate magnet or valve action are not counted as
    instrument failures.**

## 6. Design direction (to be specified)

- A single link object owns the connection: one reader, a reply slot, an
  event queue, short command serialisation. The spectrometer driver and an
  NGX valve actuator (`IValveActuator`) are both clients of it.
- Architectural consequence: the extraction line and the spectrometer own
  their transports separately today; the NGX connection must live above both.
- Infrastructure only NGX needs, removed from the Qtegra spec: secret driver
  keys in `*.local.toml` (credentials), redaction of secrets in traces and
  wire logs, a non-blocking transport read, unsolicited input in
  `SimTransport`.
- Because nothing can be proven without an instrument: an NGX simulator that
  interleaves events with replies, stress tests running acquisition, valves
  and magnet moves concurrently under the thread sanitizer, and a staged
  bring-up checklist that marks every assumption taken from untested code.

## 7. Open questions for the owner

1. Is the 2026.3.0 release the right "last instrument-proven" baseline, and
   was `93ec711d7` run on an instrument?
2. What does `SAB` do on the instrument? pychron has no comment; the
   inference is that `SAB 1` holds acquisition events during an actuation.
   Is the Isotopx protocol manual available?
3. Which labs run NGX valves through the NGX controller, and is a bench
   capture possible before the spec is written?
4. What is the real send terminator, and the real login exchange?
