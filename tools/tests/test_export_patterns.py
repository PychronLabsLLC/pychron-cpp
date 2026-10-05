"""Tests for tools/export_patterns.py (stdlib unittest; run by ctest).

The legacy files are Python pickles of pychron.lasers.pattern.patterns
classes. The tests make such pickles with stand-in classes of the same module
and class names, so neither legacy Pychron nor traits is needed. When ELCTL
names a built elctl, the exported files are also loaded by the real C++
reader (`elctl laser patterns`). When PYCHRON_LEGACY_PATTERNS names a
directory of real .lp files, those are exported too.
"""

import os
import pickle
import subprocess
import sys
import tempfile
import tomllib
import types
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import export_patterns as ep  # noqa: E402

MODULE = "pychron.lasers.pattern.patterns"


def legacy_class(name, module=MODULE):
    """A picklable stand-in for a legacy class, importable under its legacy name."""
    parts = module.split(".")
    for i in range(1, len(parts) + 1):
        sys.modules.setdefault(".".join(parts[:i]), types.ModuleType(".".join(parts[:i])))
    mod = sys.modules[module]
    if not hasattr(mod, name):
        cls = type(name, (object,), {"__module__": module})
        setattr(mod, name, cls)
    return getattr(mod, name)


def lp(directory, filename, class_name, protocol=3, **state):
    obj = legacy_class(class_name)()
    obj.__dict__.update(state)
    path = Path(directory) / filename
    path.write_bytes(pickle.dumps(obj, protocol=protocol))
    return path


class Scratch(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.src = Path(self._tmp.name) / "patterns"
        self.out = Path(self._tmp.name) / "out"
        self.src.mkdir()

    def tearDown(self):
        self._tmp.cleanup()

    def export(self, *args):
        return ep.export(self.src, self.out, *args)

    def toml(self, name):
        return tomllib.loads((self.out / f"{name}.toml").read_text())


class ReadAndConvert(Scratch):
    def test_polygon_round_trip(self):
        for protocol in (2, 3):
            path = lp(self.src, f"hept{protocol}.lp", "PolygonPattern", protocol,
                      radius=1.5, nsides=7, rotation=10.0, velocity=0.5, niterations=3,
                      beam_radius=1.0, z_pattern_enabled=False, __traits_version__="6.0")
            cls, state = ep.read_lp(path)
            self.assertEqual(cls, "PolygonPattern")
            self.assertEqual(state["nsides"], 7)
        report = self.export()
        self.assertEqual(sorted(report.written), ["hept2", "hept3"])
        self.assertEqual(self.toml("hept2"), {"kind": "polygon", "velocity": 0.5, "iterations": 3,
                                              "radius": 1.5, "nsides": 7, "rotation": 10.0})

    def test_every_mapped_class(self):
        cases = {
            "LinearPattern": ("linear", dict(length=4.0, rotation=90.0, npasses=3),
                              dict(length=4.0, rotation=90.0, npasses=3)),
            "CircularContourPattern": ("circular_contour", dict(radius=0.2, nsteps=3, percent_change=0.5),
                                       dict(radius=0.2, nsteps=3, percent_change=0.5)),
            "LineSpiralPattern": ("line_spiral", dict(radius=0.2, nsteps=3, percent_change=0.5, step_scalar=7),
                                  dict(radius=0.2, nsteps=3, percent_change=0.5, step_scalar=7)),
            "SquareSpiralPattern": ("square_spiral", dict(radius=0.3, nsteps=2, percent_change=1.0),
                                    dict(radius=0.3, nsteps=2, percent_change=1.0)),
            "RandomPattern": ("random", dict(walk_x=0.5, walk_y=0.25, npoints=20),
                              dict(walk_x=0.5, walk_y=0.25, npoints=20)),
            "RubberbandPattern": ("rubberband", dict(nominal_length=12.0, offset=0.5, rotation=45.0),
                                  dict(length=12.0, offset=0.5, rotation=45.0)),
            "RasterRubberbandPattern": ("raster", dict(nominal_length=12.0, offset=0.5, rotation=0.0, dx=0.25,
                                                       single_pass=False),
                                        dict(length=12.0, offset=0.5, rotation=0.0, dx=0.25, single_pass=False)),
            "TroughPattern": ("trough", dict(length=5.0, width=2.0, rotation=0.0, use_x=False),
                              dict(length=5.0, width=2.0, rotation=0.0, use_x=False)),
        }
        for cls, (_, state, _) in cases.items():
            lp(self.src, f"{cls}.lp", cls, velocity=2.0, niterations=1, **state)
        report = self.export()
        self.assertEqual(report.skipped, {})
        for cls, (kind, _, keys) in cases.items():
            self.assertEqual(self.toml(cls), {"kind": kind, "velocity": 2.0, "iterations": 1, **keys}, cls)

    def test_missing_attributes_take_defaults(self):
        # traits leaves an attribute still at its default out of the pickle
        lp(self.src, "bare.lp", "PolygonPattern")
        lp(self.src, "band.lp", "RubberbandPattern")
        self.export()
        self.assertEqual(self.toml("bare"), {"kind": "polygon", "velocity": 1.0, "iterations": 1,
                                             "radius": 0.5, "nsides": 3, "rotation": 0.0})
        self.assertEqual(self.toml("band")["length"], 15.0)

    def test_unmapped_classes_are_reported_and_skipped(self):
        for cls in ("ArcPattern", "SeekPattern", "DiamondPattern", "WhoKnowsPattern"):
            lp(self.src, f"{cls}.lp", cls, velocity=1.0)
        lp(self.src, "elsewhere.lp", "PolygonPattern")
        (self.src / "elsewhere.lp").write_bytes(pickle.dumps(legacy_class("Thing", "some.other.module")()))
        report = self.export()
        self.assertEqual(report.written, [])
        self.assertEqual(sorted(report.skipped), ["ArcPattern", "DiamondPattern", "SeekPattern", "WhoKnowsPattern",
                                                  "elsewhere"])
        self.assertIn("vision", report.skipped["SeekPattern"])
        self.assertIn("polygon", report.skipped["DiamondPattern"])  # says what to make instead
        self.assertFalse(self.out.exists() and any(self.out.iterdir()))

    def test_dragonfly_exports(self):
        # legacy: `duration` is the dwell at each point; how long it runs is the run's
        # duration, else manual_total_duration
        lp(self.src, "fly.lp", "DragonFlyPeakPattern", duration=0.1, manual_total_duration=45.0, base=0.4,
           perimeter_radius=2.0, saturation_threshold=0.8, velocity=1.5, spiral_kind="Square", aggressiveness=2.0,
           move_threshold=0.05, limit=10, pre_seek_delay=0.25, mask_kind="Beam", niterations=3)
        lp(self.src, "bare.lp", "DragonFlyPeakPattern")  # all at legacy's defaults: no total of its own
        report = self.export()
        self.assertEqual(report.written, ["bare", "fly"], report.skipped)
        self.assertEqual(self.toml("fly"), {"kind": "dragonfly", "velocity": 1.5, "duration": 45.0,
                                            "perimeter_radius": 2.0, "saturation_threshold": 0.8,
                                            "aggressiveness": 2.0, "move_threshold": 0.05, "spiral_base": 0.4,
                                            "spiral": "square"})
        # no duration key: it runs for the run's
        self.assertEqual(self.toml("bare"), {"kind": "dragonfly", "velocity": 1.0, "perimeter_radius": 2.5,
                                             "saturation_threshold": 0.75, "aggressiveness": 1.0,
                                             "move_threshold": 0.033, "spiral_base": 0.5, "spiral": "hexagon"})
        notes = " ".join(report.notes["fly"])
        for word in ("limit", "pre_seek_delay", "mask", "iterations", "dwell"):
            self.assertIn(word, notes)
        self.assertIn("run's duration", " ".join(report.notes["bare"]))
        self.assertNotIn("iterations", self.toml("fly"))  # a dragonfly runs once

    def test_a_dragonfly_the_reader_would_refuse_is_skipped(self):
        lp(self.src, "long.lp", "DragonFlyPeakPattern", manual_total_duration=99999.0)
        lp(self.src, "blind.lp", "DragonFlyPeakPattern", saturation_threshold=1.5)
        lp(self.src, "odd.lp", "DragonFlyPeakPattern", spiral_kind="Round")
        report = self.export()
        self.assertEqual(report.written, [])
        self.assertIn("duration", report.skipped["long"])
        self.assertIn("saturation_threshold", report.skipped["blind"])
        self.assertIn("spiral", report.skipped["odd"])

    def test_series_and_direction_are_reported(self):
        lp(self.src, "busy.lp", "LineSpiralPattern", radius=0.1, nsteps=2, percent_change=0.8, step_scalar=5,
           z_pattern_enabled=True, power_pattern_enabled=True, disable_at_end=True, direction="in")
        report = self.export()
        self.assertEqual(report.written, ["busy"])
        notes = " ".join(report.notes["busy"])
        for word in ("z series", "power series", "disable_at_end", "inward"):
            self.assertIn(word, notes)

    def test_values_the_reader_would_refuse_are_reported(self):
        lp(self.src, "flat.lp", "PolygonPattern", radius=0.0, nsides=6)
        report = self.export()
        self.assertEqual(report.written, [])
        self.assertIn("radius", report.skipped["flat"])

    def test_a_pickle_cannot_run_code(self):
        sentinel = self.src.parent / "sentinel"

        class Evil:
            def __reduce__(self):
                return (os.system, (f"touch '{sentinel}'",))

        (self.src / "evil.lp").write_bytes(pickle.dumps(Evil()))
        (self.src / "junk.lp").write_bytes(b"not a pickle at all")
        (self.src / "empty.lp").write_bytes(b"")
        report = self.export()
        self.assertFalse(sentinel.exists())
        self.assertEqual(report.written, [])
        self.assertEqual(sorted(report.skipped), ["empty", "evil", "junk"])

    def test_a_pickle_cannot_hang_or_crash_the_export(self):
        # values a pattern never has: each is refused by its type, never printed or converted
        bomb = ()
        for _ in range(60):
            bomb = (bomb, bomb)  # printing it would take 2**60 characters
        deep = []
        for _ in range(20000):
            deep = [deep]
        lp(self.src, "a_bomb.lp", "PolygonPattern", radius=bomb)
        lp(self.src, "b_huge.lp", "PolygonPattern", radius=10 ** 400)
        lp(self.src, "c_deep.lp", "PolygonPattern", radius=deep)
        lp(self.src, "d_text.lp", "PolygonPattern", nsides="6")
        lp(self.src, "e_flag.lp", "TroughPattern", use_x="yes")
        odd = legacy_class("PolygonPattern")()
        odd.__dict__.update({"radius": 1.0})
        raw = pickle.dumps(odd, protocol=2)
        # the same pickle with a non-text key in its state
        (self.src / "f_keys.lp").write_bytes(raw.replace(b"X\x06\x00\x00\x00radius", b"K\x01"))
        lp(self.src, "z_good.lp", "PolygonPattern", radius=1.0, nsides=5)
        report = self.export()
        self.assertEqual(report.written, ["f_keys", "z_good"], report.skipped)  # the odd key is ignored
        self.assertEqual(sorted(report.skipped), ["a_bomb", "b_huge", "c_deep", "d_text", "e_flag"])
        for name in ("a_bomb", "c_deep"):
            self.assertIn("radius", report.skipped[name])
            self.assertLess(len(report.skipped[name]), 300)
        self.assertEqual(self.toml("z_good")["nsides"], 5)

    def test_a_pattern_of_too_many_points_is_not_written(self):
        lp(self.src, "long.lp", "CircularContourPattern", nsteps=10, niterations=30)
        lp(self.src, "fine.lp", "RasterRubberbandPattern", nominal_length=15.0, dx=0.00000002)
        report = self.export()
        self.assertEqual(report.written, [])
        self.assertIn("points", report.skipped["long"])
        self.assertIn("dx", report.skipped["fine"])

    def test_python2_pickles_are_read(self):
        # protocol 2 from Python 2: classic copy_reg reconstruction, byte-string keys
        raw = (b"\x80\x02c" + MODULE.encode() + b"\nPolygonPattern\nq\x00)\x81q\x01}q\x02"
               b"(U\x06radiusq\x03G?\xf8\x00\x00\x00\x00\x00\x00U\x06nsidesq\x04K\x05ub.")
        (self.src / "old.lp").write_bytes(raw)
        report = self.export()
        self.assertEqual(report.written, ["old"], report.skipped)
        self.assertEqual(self.toml("old")["radius"], 1.5)
        self.assertEqual(self.toml("old")["nsides"], 5)

    def test_old_traits_pickles_are_read(self):
        # older traits pickled a HasTraits object as traits.traits.__newobj__(cls), then its state
        for i, module in enumerate(("traits.traits", "traits.has_traits", "enthought.traits.traits", "copy_reg")):
            raw = (b"\x80\x02c" + module.encode() + b"\n__newobj__\nq\x00c" + MODULE.encode() +
                   b"\nPolygonPattern\nq\x01\x85q\x02Rq\x03}q\x04"
                   b"(U\x06radiusq\x05G@\x00\x00\x00\x00\x00\x00\x00U\x06nsidesq\x06K\x04ub.")
            (self.src / f"old{i}.lp").write_bytes(raw)
        report = self.export()
        self.assertEqual(report.skipped, {})
        self.assertEqual(report.written, ["old0", "old1", "old2", "old3"])
        self.assertEqual(self.toml("old0")["radius"], 2.0)
        self.assertEqual(self.toml("old0")["nsides"], 4)

    def test_does_not_overwrite_without_force(self):
        lp(self.src, "p.lp", "PolygonPattern", radius=1.0)
        self.out.mkdir()
        (self.out / "p.toml").write_text("# mine\n")
        report = self.export()
        self.assertEqual(report.written, [])
        self.assertIn("exists", report.skipped["p"])
        self.assertEqual((self.out / "p.toml").read_text(), "# mine\n")
        report = ep.export(self.src, self.out, True)
        self.assertEqual(report.written, ["p"])

    def test_names_become_plain_file_names(self):
        lp(self.src, "Square 9 hole 3 beam.lp", "PolygonPattern", nsides=4)
        report = self.export()
        self.assertEqual(report.written, ["Square 9 hole 3 beam"])
        self.assertTrue((self.out / "Square 9 hole 3 beam.toml").exists())

    def test_command_line(self):
        lp(self.src, "p.lp", "PolygonPattern", radius=1.0)
        lp(self.src, "s.lp", "SeekPattern")
        done = subprocess.run([sys.executable, str(Path(ep.__file__)), str(self.src), str(self.out)],
                              capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("wrote p.toml", done.stdout)
        self.assertIn("skipped s", done.stdout)
        none = subprocess.run([sys.executable, str(Path(ep.__file__)), str(self.src / "nope"), str(self.out)],
                              capture_output=True, text=True)
        self.assertNotEqual(none.returncode, 0)


class AgainstTheRealReader(Scratch):
    @unittest.skipUnless(os.environ.get("ELCTL"), "ELCTL (a built elctl) is not set")
    def test_written_files_load(self):
        lp(self.src, "hept.lp", "PolygonPattern", radius=1.5, nsides=7)
        lp(self.src, "ras.lp", "RasterRubberbandPattern", nominal_length=12.0, offset=0.5, dx=0.25)
        lp(self.src, "spi.lp", "LineSpiralPattern", radius=0.2, nsteps=3, percent_change=0.5, step_scalar=7)
        lp(self.src, "walk.lp", "RandomPattern", walk_x=0.5, walk_y=0.5, npoints=5)
        self.export()
        lab = self.out.parent / "lab"
        lab.mkdir()
        self.out.rename(lab / "patterns")
        done = subprocess.run([os.environ["ELCTL"], "laser", "patterns", "--lab", str(lab)],
                              capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        lp(self.src, "fly.lp", "DragonFlyPeakPattern", manual_total_duration=30.0)
        self.export()
        (lab / "patterns").mkdir(exist_ok=True)
        (self.out / "fly.toml").rename(lab / "patterns" / "fly.toml")
        done = subprocess.run([os.environ["ELCTL"], "laser", "patterns", "--lab", str(lab)],
                              capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertIn("fly  dragonfly", done.stdout)
        for name in ("hept  polygon", "ras  raster", "spi  line_spiral", "walk  random"):
            self.assertIn(name, done.stdout)

    @unittest.skipUnless(os.environ.get("PYCHRON_LEGACY_PATTERNS"), "PYCHRON_LEGACY_PATTERNS is not set")
    def test_real_lab_files(self):
        report = ep.export(Path(os.environ["PYCHRON_LEGACY_PATTERNS"]), self.out)
        print("\nwritten:", report.written, "\nskipped:", report.skipped, "\nnotes:", report.notes)
        self.assertTrue(report.written)
        for name, why in report.skipped.items():
            self.assertNotIn("cannot be read", why, name)


if __name__ == "__main__":
    unittest.main()
