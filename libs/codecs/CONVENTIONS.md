# Codec conventions

A codec answers two questions for one vendor protocol: *what bytes does the
device want* and *what do its replies mean*. It never touches I/O.

## Rules

1. **Pure.** No I/O, threads, clocks, sleeps, logging, config or globals.
   Every function is deterministic in its arguments.
2. **Layout.** One vendor per namespace and directory:
   `include/pychron/codecs/<vendor>.hpp`, `src/<vendor>.cpp`,
   namespace `pychron::codec::<vendor>`. Shared helpers live only in
   `pychron/codecs/codec.hpp`; add there rather than copy between vendors.
3. **Encode** returns `codec::Command` — the tx bytes plus the `ReadSpec`
   that frames the reply (`std::nullopt` when the device sends nothing).
   Terminators, checksums and handshakes (e.g. ENQ/ACK) are encoded here.
   Invalid arguments (channel out of range) are `ErrorKind::Config`.
4. **Decode** takes the complete reply bytes and returns `Result<T>`. Any
   reply it cannot accept — bad checksum, NAK, wrong length, unparsable
   number, device-reported error — is `ErrorKind::Protocol`, built with
   `codec::protocol_error(what, reply)` so the bytes appear in the message.
   Never throw; never return a sentinel value.
5. **No identity.** Codecs leave `Error::device` empty; the driver (via
   `Device::observe`) attributes errors to itself.
6. **Units.** Decode to plain values exactly as the device reports them (e.g. pressure in the gauge's configured unit). Unit conversion is the
   driver's or manager's job, and is explicit.
7. **Tests** live in `tests/codecs/test_<vendor>.cpp`, use literal byte
   strings taken from the vendor manual or captured traces, and cover: every
   command encoding, a good reply per decoder, checksum/NAK/garbage/truncated
   frames, and boundary values.

## Driver side

A driver composes a `Transport&` and its codec:

```cpp
Result<double> MaxiGauge::read_pressure() {
  auto cmd = codec::maxigauge::read_pressure(channel_);
  if (!cmd) return observe(Result<double>(fail(cmd.error())));
  auto reply = transport_.exchange(cmd->tx, *cmd->reply);
  if (!reply) return observe(Result<double>(fail(reply.error())));
  return observe(codec::maxigauge::decode_pressure(*reply));
}
```

Transport handles timeouts, retries and bus serialization; the codec handles
meaning; the driver handles sequencing and which channel/address to use.
