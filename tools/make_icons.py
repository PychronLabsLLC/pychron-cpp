#!/usr/bin/env python3
"""Packs the application icon into the installers' icon files.

The icon is painted in code (apps/pychron-ui/src/brand.cpp); this turns its
rendered sizes into files the platforms want:

    pychron-ui --write-icons /tmp/icons
    python3 tools/make_icons.py /tmp/icons packaging/icons

writes packaging/icons/pychron.icns (macOS bundle), pychron.ico (Windows
program and installer) and pychron.png (Linux, 512 px). Both containers hold
PNG images, which every supported macOS and Windows reads. Run it again after
changing the icon and commit the results.
"""

import shutil
import struct
import sys
from pathlib import Path

# ICNS entry types holding PNG data, by pixel size (the @2x types share sizes).
ICNS_TYPES = [
    (b"icp4", 16), (b"icp5", 32), (b"icp6", 64),
    (b"ic07", 128), (b"ic08", 256), (b"ic09", 512), (b"ic10", 1024),
    (b"ic11", 32), (b"ic12", 64), (b"ic13", 256), (b"ic14", 512),
]
ICO_SIZES = [16, 24, 32, 48, 64, 128, 256]


def png(src: Path, size: int) -> bytes:
    data = (src / f"pychron-{size}.png").read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"pychron-{size}.png is not a PNG")
    return data


def icns(src: Path) -> bytes:
    body = b"".join(t + struct.pack(">I", 8 + len(d)) + d for t, d in ((t, png(src, s)) for t, s in ICNS_TYPES))
    return b"icns" + struct.pack(">I", 8 + len(body)) + body


def ico(src: Path) -> bytes:
    images = [png(src, s) for s in ICO_SIZES]
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries, payload = b"", b""
    for size, data in zip(ICO_SIZES, images):
        dim = 0 if size >= 256 else size  # 0 means 256
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset + len(payload))
        payload += data
    return header + entries + payload


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: make_icons.py <dir from pychron-ui --write-icons> <output dir>")
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    (out / "pychron.icns").write_bytes(icns(src))
    (out / "pychron.ico").write_bytes(ico(src))
    shutil.copyfile(src / "pychron-512.png", out / "pychron.png")
    for name in ("pychron.icns", "pychron.ico", "pychron.png"):
        print(out / name, (out / name).stat().st_size, "bytes")


if __name__ == "__main__":
    main()
