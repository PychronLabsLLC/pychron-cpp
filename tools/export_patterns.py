#!/usr/bin/env python3
"""Export legacy Pychron laser patterns (.lp pickles) to pychron-cpp TOML.

    python3 tools/export_patterns.py <dir of .lp files> <out dir> [--force]

Legacy Pychron stores a pattern as a Python pickle of a
pychron.lasers.pattern.patterns class. This reads the pickle without legacy
Pychron (or traits, or numpy) installed, and without running anything in it:
every class a pickle names is replaced by an inert stand-in that only keeps
the attributes it is given. Each pattern becomes <out dir>/<name>.toml, the
file `<lab>/patterns` holds (docs/dev_setup.md).

Not exported, and said so: arc and seek patterns (pychron-cpp has no arc move
and no seek), the deprecated diamond, z and power series, a spiral's inward
direction, and a dragonfly's limit, delay and mask.
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
            for key, value in state.items():
                key = _text(key)
                if type(key) is str:  # anything else is not an attribute name
                    self.__dict__[key] = value

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
# A dragonfly follows the glow for a time: it has a duration and no iterations.
_DRAGONFLY = {
    "velocity": ("velocity", 1.0, 0.0, _MM, False),
    "duration": ("duration", 0.1, 0.0, 3600.0, False),
    "perimeter_radius": ("perimeter_radius", 2.5, 0.0, _MM, False),
    "saturation_threshold": ("saturation_threshold", 0.75, 0.0, 1.0, False),
    "spiral_base": ("base", 0.5, 0.0, _MM, False),
}
_NOT_EXPORTED = {
    "ArcPattern": "an arc needs a controller arc move, which pychron-cpp does not have",
    "SeekPattern": "seek is driven by vision, not a pattern of points",
    "DiamondPattern": "deprecated in legacy Pychron: make it a polygon with nsides = 4",
}


MAX_POINTS = 10000  # as the reader's kMaxPatternPoints


def _number(key, attribute, value):
    """`value` as a float. Only a plain number is one: nothing else is converted or printed
    (a pickle can hold a value whose text is astronomically long)."""
    if type(value) not in (int, float) or (type(value) is int and abs(value) > 10 ** 12):
        raise ValueError(f"{key} ({attribute}) is not a number a pattern can have (a {type(value).__name__})")
    return float(value)


def _value(key, spec, state):
    """The value for `key`, as TOML text; raises ValueError if the reader would refuse it."""
    attribute, default, low, high, low_allowed = spec
    value = state.get(attribute, default)
    if isinstance(default, bool):
        if type(value) not in (bool, int):
            raise ValueError(f"{key} ({attribute}) is not true or false (a {type(value).__name__})")
        return "true" if value else "false"
    number = _number(key, attribute, value)
    if not math.isfinite(number) or number < low or number > high or (number == low and not low_allowed):
        raise ValueError(f"{key} ({attribute} = {number!r}) is outside what a pattern allows")
    if isinstance(default, int):
        if number != int(number):
            raise ValueError(f"{key} ({attribute} = {number!r}) is not a whole number")
        return str(int(number))
    return repr(number)


def _points(kind, values):
    """How many points one pass of the pattern has (as the reader counts them)."""
    if kind == "polygon":
        return values["nsides"] + 1
    if kind == "linear":
        return 2 * values["npasses"]
    if kind == "circular_contour":
        return 37 * values["nsteps"]
    if kind == "square_spiral":
        return 4 * values["nsteps"] + 1
    if kind == "line_spiral":
        turns = values["nsteps"]
        count = sum(max(2 * t + values["step_scalar"], 0) - (0 if t == turns - 1 else 1)
                    for t in range(turns) if 2 * t + values["step_scalar"] > 1)
        return max(count, 1)
    if kind == "random":
        return values["npoints"]
    if kind == "raster":
        box = values["length"] + 2 * values["offset"]
        if values["dx"] > box:
            raise ValueError("dx is wider than the box it rasters")
        steps = math.floor(box / values["dx"] + 1e-9)
        if steps > 4 * MAX_POINTS:
            raise ValueError("dx is so fine the raster has too many points")
        steps += steps % 2
        return (steps + 2) if values["single_pass"] else 2 * (steps + 2) + 1
    return 5  # rubberband, trough


def to_toml(class_name, state):
    """(TOML text or None, notes). None: the pattern is not exported; notes say why."""
    if class_name in _NOT_EXPORTED:
        return None, [_NOT_EXPORTED[class_name]]
    if class_name in ("DragonFlyPeakPattern", "DragonFlyPattern"):
        lines = ['kind = "dragonfly"']
        try:
            for key, spec in _DRAGONFLY.items():
                lines.append(f"{key} = {_value(key, spec, state)}")
        except ValueError as error:
            return None, [str(error)]
        notes = []
        for attribute, what in (("limit", "its limit"), ("pre_seek_delay", "its pre_seek_delay"),
                                ("mask_kind", "its mask"), ("custom_mask_radius", "its mask radius")):
            if attribute in state:
                notes.append(f"{what} ({attribute}) is not carried over")
        if state.get("niterations", 1) not in (1, None):
            notes.append("its iterations are not carried over: a dragonfly runs once, for its duration")
        return "\n".join(lines) + "\n", notes
    if class_name not in _KINDS:
        return None, [f"{class_name} is not a pattern this tool knows"]
    kind, keys = _KINDS[class_name]
    lines = [f'kind = "{kind}"']
    try:
        values = {}
        for key, spec in {**_COMMON, **keys}.items():
            text = _value(key, spec, state)
            lines.append(f"{key} = {text}")
            if text in ("true", "false"):
                values[key] = text == "true"
            else:
                values[key] = int(text) if isinstance(spec[1], int) else float(text)
        points = int(_points(kind, values) * values["iterations"]) + 1
        if points > MAX_POINTS:
            raise ValueError(f"it has {points} points over its iterations; a pattern may have {MAX_POINTS}")
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
            text, notes = to_toml(class_name, state)
        except ValueError as error:
            report.skipped[name] = str(error)
            continue
        except Exception as error:  # whatever a file holds, the next one is still exported
            report.skipped[name] = f"cannot be exported ({type(error).__name__})"
            continue
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
