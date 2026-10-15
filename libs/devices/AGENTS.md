# Device drivers (`libs/devices`): agent rules

Part of the agent guide: root `AGENTS.md` applies in full, this adds what is
only this component's. Kept compressed, as that file is.

## Which command may be sent twice

- Every wire call of a driver goes through `Device::call` (or `relay`, same
  path unobserved, when one operation is several calls observed once), with
  a `Repeat` (`link.hpp`). Through the transport's `Link` a `Safe` call is
  reconnected and repeated; an `Unsafe` one is reconnected, never repeated,
  and returns `code = "outcome_unknown"`: caller reads back.
- Marking rule (spec `docs/superpowers/specs/2026-10-09-executor-hardening-design.md`
  4.2): command that sets absolute target (valve open, setpoint 450, stage to
  hole 7, IG on) or reads is `Safe`. Command whose second arrival does more
  (toggle, step, `StartAcq`, `Laser.Fire`, relative move, read that clears a
  counter or takes an error off a queue) is `Unsafe`.
- Unit repeated is driver's transaction (`transact`) or sequence under its
  own mutex, never one write of it: Chromium action + confirm, ProXR bank
  select + relay, Lakeshore `RANGE` + `SETP` + `SETP?`, NGX `SAB 1` ...
  `SAB 0`. Transaction mixing `Safe` and `Unsafe` (Agilent route, then
  `SYST:ERR?`): wrap whole as `Safe`, and the `Unsafe` part marks its
  transient failure with `outcome_unknown_if_transient`; `Link` then does not
  repeat.
- `Resend` handed to op goes to every `exchange` in it: transport's own
  `retries` resend only `Safe` calls.
- Driver's header carries its table as comment above class: `Safe` row,
  `Unsafe` row.
- New command, or command changing class, without a row in
  `tests/devices/test_repeat_tables.cpp` (`RepeatTable.<Driver>`) is not
  done. Row: link taken down 3 s just after command reached instrument;
  `Safe` seen once more than on clean link and call succeeds; `Unsafe` seen
  once, call returns `outcome_unknown`.
- Driver constructor takes link: `DeviceOptions::link`, or trailing
  `Link* link = nullptr` where constructor takes only a clock. No link: op
  runs once, as before.
