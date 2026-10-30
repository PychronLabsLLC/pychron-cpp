#!/bin/sh
# The container's entry point: `elctl sim serve` on the lab mounted at /lab,
# with the addresses of spec section 6, phase D. Arguments are passed on.
#
#   SIM_ADVERTISE   host written to the endpoints file (default 127.0.0.1):
#                   what a client outside the container connects to
#   SIM_PORT_BASE   first port (default 47000), one per served transport
set -eu

line=/lab/extraction_line.toml
if [ ! -f "$line" ]; then
  echo "sim-serve: no $line: mount the lab at /lab" >&2
  exit 2
fi

set -- --config "$line" --bind 0.0.0.0 \
  --port-base "${SIM_PORT_BASE:-47000}" \
  --advertise "${SIM_ADVERTISE:-127.0.0.1}" \
  --endpoints /endpoints/sim-endpoints.toml "$@"
if [ -f /lab/spectrometer.toml ]; then
  set -- --spectrometer /lab/spectrometer.toml "$@"
fi
# exec: elctl is process 1 and gets the stop signal.
exec /usr/local/bin/elctl sim serve "$@"
