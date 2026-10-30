# `elctl sim serve` in a Linux image: the simulated lab on the wire, for a
# machine that has no way to build it, or a rehearsal on another host. See
# packaging/sim/README.md. Not built in CI; nothing depends on it.
#
# Built from the repository root (compose.yaml does this):
#   docker build -f packaging/sim/Dockerfile -t pychron-sim .

FROM ubuntu:24.04 AS build
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       ca-certificates g++-14 git ninja-build python3 python3-pip \
    && pip3 install --break-system-packages cmake
ARG SIM_BUILD_JOBS=2
WORKDIR /src
COPY . .
# Only what `sim serve` needs: no UI, tests, store or embedded Python. gcc 14,
# as CI; warnings are CI's business, not an image's.
RUN cmake -S . -B /build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14 \
      -DBUILD_UI=OFF -DBUILD_TESTS=OFF \
      -DPYCHRON_PERSISTENCE=OFF -DPYCHRON_SCRIPTING=OFF \
      -DPYCHRON_VISION_OPENCV=OFF -DPYCHRON_PUBLISH=OFF \
      -DPYCHRON_WARNINGS_AS_ERRORS=OFF \
    && cmake --build /build --target elctl -j "$SIM_BUILD_JOBS"

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends libstdc++6 tzdata \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /build/apps/elctl/elctl /usr/local/bin/elctl
COPY packaging/sim/serve.sh /usr/local/bin/sim-serve
# Where elctl keeps its log: any user the container runs as can write it.
ENV HOME=/tmp
# elctl stops cleanly on SIGINT, removing the endpoints file.
STOPSIGNAL SIGINT
ENTRYPOINT ["/usr/local/bin/sim-serve"]
