# The simulated lab in a container

`elctl sim serve` in a Linux image, for a rehearsal on a machine without a
build of `elctl`, or with the simulated lab on another host. What sim serve is
and what it exercises: "The simulated lab on the wire" in
[docs/simulator.md](../../docs/simulator.md). The image is not built in CI and
nothing depends on it.

## Run it

From the repository root, with `PYCHRON_LAB` the folder holding the lab's
`extraction_line.toml` (and, if it has them, `canvas.toml`, `sim.toml`,
`spectrometer.toml` and `tables/`):

```bash
PYCHRON_LAB=$PWD/mylab SIM_ENDPOINTS_DIR=$PWD/mylab-endpoints docker compose -f packaging/sim/compose.yaml up --build
```

The lab is mounted read-only at `/lab`. The first build fetches the
dependencies and takes a while; a small Docker VM can be short of memory, in
which case pass `--build-arg SIM_BUILD_JOBS=1` to `docker build` first.

The container prints its table of transports and ports (from 47000), and
writes `sim-endpoints.toml` to `SIM_ENDPOINTS_DIR`. Point a client at it from
the host, on the same files:

```bash
elctl -c mylab/extraction_line.toml --sim-endpoints mylab-endpoints/sim-endpoints.toml --sim-endpoints-any state
```

`--sim-endpoints-any` is needed because the server knows the line as
`/lab/extraction_line.toml`: it skips the check that the file serves the path
the client loaded, so make sure it is the same lab. The same two options on
`exp run`, or on `pychron-ui`, run a queue against it. `docker compose down`,
or Ctrl-C, stops it cleanly and removes the file.

## From another machine

The emulators accept any login and have no authentication, so ports are
published on the host's loopback unless told otherwise. To reach them from
another machine on a network you trust:

```bash
PYCHRON_LAB=$PWD/mylab SIM_PUBLISH_ADDRESS=0.0.0.0 SIM_ADVERTISE=sim-host.example docker compose -f packaging/sim/compose.yaml up
```

`SIM_ADVERTISE` is the host name written to the endpoints file; copy the file
to the client.

## Limits

- 32 ports are published, 47000 to 47031. A lab with more served transports
  needs the range in `compose.yaml` widened.
- `SIM_UID` and `SIM_GID` (default 1000) are who the endpoints file is written
  as: give yours if the folder is not writable by 1000.
- The image holds `elctl` only, built without the store, so it serves and
  nothing else.
