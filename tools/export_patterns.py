#!/usr/bin/env python3
"""Export legacy Pychron laser patterns (.lp pickles) to pychron-cpp TOML.

    python3 tools/export_patterns.py <dir of .lp files> <out dir> [--force]

Legacy Pychron stores a pattern as a Python pickle of a
pychron.lasers.pattern.patterns class. This reads the pickle without legacy
Pychron (or traits, or numpy) installed, and without running anything in it:
every class a pickle names is replaced by an inert stand-in that only keeps
the attributes it is given. Each pattern becomes <out dir>/<name>.toml, the
file `<lab>/patterns` holds (docs/dev_setup.md).

Not exported, and said so: arc, seek and dragonfly patterns (pychron-cpp has
no arc move, and the vision-driven ones are not patterns of points), the
deprecated diamond, z and power series, and a spiral's inward direction.
Run it once per lab, then check the result with `elctl laser patterns`.
"""

import io
import math
import pickle
import sys
from dataclasses import dataclass, field
from pathlib import Path

PATTERN_MODULE_PREFIX = "pychron.lasers.pattern"


class _Inert:
    """Stands in for anything a pickle names: keeps what it is given, runs nothing."""

    _module = ""
    _name = ""

    def __init__(self, *args, **kwargs):
        self._args = args

    def __setstate__(self, state):
        if isinstance(state, tuple) and len(state) == 2 and isinstance(state[1], dict):
            state = {**(state[0] or {}), **state[1]}  # (dict state, slots state)
        if isinstance(state, dict):
            self.__dict__.update({_text(k): v for k, v in state.items()})

    def __call__(self, *args, **kwargs):
        return _Inert()

    # containers a legacy object may be asked to fill
    def append(self, *_):
        pass

    def extend(self, *_):
        pass

    def __setitem__(self, *_):
        pass


def _text(key):
    return key.decode("latin1") if isinstance(key, bytes) else key


def _reconstruct(cls, *_):
    """An empty instance of `cls`, itself a stand-in: nothing real is built."""
    return cls() if isinstance(cls, type) and issubclass(cls, _Inert) else _Inert()


class _SafeUnpickler(pickle.Unpickler):
    """Resolves no real class or function, whatever the pickle asks for."""

    def __init__(self, file):
        super().__init__(file, encoding="latin1")
        self._classes = {}

    def find_class(self, module, name):
        # The ways a pickle says "an empty instance of this class": copy_reg's
        # of old pickles, and the __newobj__ old traits used for HasTraits.
        if name in ("_reconstructor", "__newobj__"):
            return _reconstruct
        key = (module, name)
        if key not in self._classes:
            self._classes[key] = type(name, (_Inert,), {"_module": module, "_name": name})
        return self._classes[key]

    def persistent_load(self, pid):
        return _Inert()


def read_lp(path):
    """(class name, attributes) of the pattern pickled in `path`.

    Raises ValueError if the file is not a pickle of a legacy pattern.
    """
    try:
        obj = _SafeUnpickler(io.BytesIO(Path(path).read_bytes())).load()
    except Exception as error:  # a pickle can fail in any way at all
        raise ValueError(f"cannot be read as a pickle ({type(error).__name__}: {error})") from None
    if not isinstance(obj, _Inert) or not obj._module.startswith(PATTERN_MODULE_PREFIX):
        where = f"{obj._module}.{obj._name}" if isinstance(obj, _Inert) else type(obj).__name__
        raise ValueError(f"is not a legacy laser pattern (it holds a {where})")
    state = {k: v for k, v in vars(obj).items() if not k.startswith("_")}
    return obj._name, state


# key: (legacy attribute, legacy default, low, high, low itself allowed)
_ANY = 1e6
_MM = 1000.0
_COMMON = {
    "velocity": ("velocity", 1.0, 0.0, _MM, False),
    "iterations": ("niterations", 1, 1, 200, True),
}
_RADIUS_CONTOUR = {
    "radius": ("radius", 0.1, 0.0, _MM, False),
    "nsteps": ("nsteps", 2, 1, 10, True),
    "percent_change": ("percent_change", 0.8, 0.0, 100.0, False),
}
_BAND = {
    "length": ("nominal_length", 15.0, 0.0, _MM, False),
    "offset": ("offset", 0.0, 0.0, _MM, True),
    "rotation": ("rotation", 0.0, -_ANY, _ANY, True),
}
_KINDS = {
    "PolygonPattern": ("polygon", {
        "radius": ("radius", 0.5, 0.0, _MM, False),
        "nsides": ("nsides", 3, 3, 200, True),
        "rotation": ("rotation", 0.0, -_ANY, _ANY, True),
    }),
    "LinearPattern": ("linear", {
        "length": ("length", 0.0, 0.0, _MM, False),
        "rotation": ("rotation", 0.0, -_ANY, _ANY, True),
        "npasses": ("npasses", 1, 1, 100, True),
    }),
    "CircularContourPattern": ("circular_contour", _RADIUS_CONTOUR),
    "LineSpiralPattern": ("line_spiral", {**_RADIUS_CONTOUR, "step_scalar": ("step_scalar", 5, 1, 20, True)}),
    "SquareSpiralPattern": ("square_spiral", _RADIUS_CONTOUR),
    "RandomPattern": ("random", {
        "walk_x": ("walk_x", 1.0, 0.0, _MM, False),
        "walk_y": ("walk_y", 1.0, 0.0, _MM, False),
        "npoints": ("npoints", 10, 1, 50, True),
    }),
    "RubberbandPattern": ("rubberband", _BAND),
    "RasterRubberbandPattern": ("raster", {
        **_BAND,
        "dx": ("dx", 0.5, 0.0, _MM, False),
        "single_pass": ("single_pass", True, None, None, True),
    }),
    "TroughPattern": ("trough", {
        "length": ("length", 10.0, 0.0, _MM, False),
        "width": ("width", 10.0, 0.0, _MM, False),
        "rotation": ("rotation", 0.0, -_ANY, _ANY, True),
        "use_x": ("use_x", True, None, None, True),
    }),
}
_NOT_EXPORTED = {
    "ArcPattern": "an arc needs a controller arc move, which pychron-cpp does not have",
    "SeekPattern": "seek is driven by vision, not a pattern of points",
    "DragonFlyPeakPattern": "dragonfly is driven by vision, not a pattern of points",
    "DragonFlyPattern": "dragonfly is driven by vision, not a pattern of points",
    "DiamondPattern": "deprecated in legacy Pychron: make it a polygon with nsides = 4",
}


def _value(key, spec, state):
    """The value for `key`, as TOML text; raises ValueError if the reader would refuse it."""
    attribute, default, low, high, low_allowed = spec
    value = state.get(attribute, default)
    if isinstance(default, bool):
        if not isinstance(value, (bool, int)):
            raise ValueError(f"{key} ({attribute} = {value!r}) is not true or false")
        return "true" if value else "false"
    try:
        number = float(value)
    except (TypeError, ValueError):
        raise ValueError(f"{key} ({attribute} = {value!r}) is not a number") from None
    if not math.isfinite(number) or number < low or number > high or (number == low and not low_allowed):
        raise ValueError(f"{key} ({attribute} = {value!r}) is outside what a pattern allows")
    if isinstance(default, int):
        if number != int(number):
            raise ValueError(f"{key} ({attribute} = {value!r}) is not a whole number")
        return str(int(number))
    return repr(number)


def to_toml(class_name, state):
    """(TOML text or None, notes). None: the pattern is not exported; notes say why."""
    if class_name in _NOT_EXPORTED:
        return None, [_NOT_EXPORTED[class_name]]
    if class_name not in _KINDS:
        return None, [f"{class_name} is not a pattern this tool knows"]
    kind, keys = _KINDS[class_name]
    lines = [f'kind = "{kind}"']
    try:
        for key, spec in {**_COMMON, **keys}.items():
            lines.append(f"{key} = {_value(key, spec, state)}")
        if kind == "raster":
            dx = float(state.get("dx", 0.5))
            box = float(state.get("nominal_length", 15.0)) + 2 * float(state.get("offset", 0.0))
            if dx > box:
                raise ValueError("dx is wider than the box it rasters")
    except ValueError as error:
        return None, [str(error)]
    notes = []
    if state.get("z_pattern_enabled"):
        notes.append("its z series is not carried over")
    if state.get("power_pattern_enabled"):
        notes.append("its power series is not carried over")
    if state.get("disable_at_end"):
        notes.append("disable_at_end is not carried over (the run switches the laser off)")
    if state.get("direction") == "in":
        notes.append("its inward direction is not carried over: the spiral runs outward")
    if not state.get("xy_pattern_enabled", True):
        notes.append("its xy pattern was switched off in legacy Pychron; it is exported switched on")
    return "\n".join(lines) + "\n", notes


@dataclass
class Report:
    written: list = field(default_factory=list)   # pattern names
    skipped: dict = field(default_factory=dict)   # name -> why
    notes: dict = field(default_factory=dict)     # name -> [what was dropped]


def export(source, out, force=False):
    """Exports every .lp in `source` to `out`; returns a Report. Raises ValueError for a missing source."""
    source, out = Path(source), Path(out)
    if not source.is_dir():
        raise ValueError(f"{source} is not a directory")
    report = Report()
    for path in sorted(p for p in source.iterdir() if p.suffix == ".lp" and not p.name.startswith(".")):
        name = path.stem
        try:
            class_name, state = read_lp(path)
        except ValueError as error:
            report.skipped[name] = str(error)
            continue
        text, notes = to_toml(class_name, state)
        if text is None:
            report.skipped[name] = "; ".join(notes)
            continue
        target = out / f"{name}.toml"
        if target.exists() and not force:
            report.skipped[name] = f"{target} exists (use --force to replace it)"
            continue
        out.mkdir(parents=True, exist_ok=True)
        target.write_text(f"# Exported from legacy Pychron's {path.name} ({class_name}).\n" + text)
        report.written.append(name)
        if notes:
            report.notes[name] = notes
    return report


def main(argv):
    args = [a for a in argv if a != "--force"]
    if len(args) != 2 or any(a.startswith("-") for a in args):
        print(__doc__.strip().split("\n\n")[0] + "\n\n    " + __doc__.strip().split("\n\n")[1].strip(), file=sys.stderr)
        return 2
    try:
        report = export(args[0], args[1], "--force" in argv)
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    for name in report.written:
        print(f"wrote {name}.toml")
        for note in report.notes.get(name, []):
            print(f"  note: {note}")
    for name, why in report.skipped.items():
        print(f"skipped {name}: {why}")
    print(f"{len(report.written)} written, {len(report.skipped)} skipped")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
