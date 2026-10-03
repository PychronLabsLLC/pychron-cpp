# Vision fixtures

Fixture frames let the finder be checked against real images. They are the only
ground truth the vision library has, so read this page before trusting them.

## Layout

```
tests/vision/data/<case>/
  0001.pgm ...
  case.toml
```

```toml
provenance = "screen_recording"   # synthetic | screen_recording | raw
mode = "hole"                     # hole | glow
expected_radius_px = 11.0
tolerance_px = 4.0
channel = "luma"                  # informational: luma | r | g | b
note = "source recording and crop"  # informational
[[frames]]
file = "0001.pgm"
center_px = [40.0, 40.0]          # in the pixels of this (cropped) frame
skip = true                       # optional: known failure, tests ignore the frame
```

Frames are binary P5 PGM (maxval up to 65535; above 255 two bytes, big-endian).
`FrameRecorder` writes this layout, so the first hardware session produces
fixtures. `RecordedSource` replays a case in file order (`seq` from 1).

## Provenance levels

| Level | Meaning | May be used for |
|---|---|---|
| `synthetic` | rendered by `synth.hpp` or written by `FrameRecorder` from a simulation | regression of the replay path; tight tolerances only against the scene's own truth |
| `screen_recording` | cropped from a screen recording of the old UI: overlays, compression artefacts | smoke checks with loose tolerances; never to tune constants |
| `raw` | frames from the camera, no overlays | tight tolerances, tuning |

## External sequences

Full sequences stay outside the repo. Set `PYCHRON_VISION_FIXTURES` to a
directory holding case directories in the layout above; `ExternalCasesWithinTolerance`
runs the same assertion on them and is skipped when the variable is unset.

## Extracting frames

Generic form, for future recordings:

```
ffmpeg -ss <t> -i in.mov -frames:v 1 -vf "crop=w:h:x:y,format=gray" out.pgm
```

Use `extractplanes=g` (or `r`, `b`) in place of `format=gray` for one colour
channel. `ffmpeg` is for future use; it was not available when the committed
frames were made.

### How the committed frames were made (reproducible)

1. One frame was taken from each recording at time `t` (the scratch names are
   `<recording><n>_<t>`: `ac1_3` is AutoCenter1.mov at t = 3 s, `df0_35` is
   DragonFly.mov at t = 35 s, `df1_30` is dragonfly1.mov at t = 30 s, `ac2_18`
   is AutoCenter2.mov at t = 18 s).
2. The video pane was cropped from the movie frame (x, y, w, h in full-movie
   pixels): AutoCenter1.mov 426,402,591,444; AutoCenter2.mov 0,0,216,170 (the
   whole frame); DragonFly.mov 937,337,875,658; dragonfly1.mov 974,337,873,658.
3. Luma is the Core Graphics device-grey conversion; the `_g` / `_b` variants
   are the raw green / blue channel.
4. The implementer then cut the sub-crops listed below out of the pane with
   `crop()` and wrote them with `write_pgm`. **The sub-crop sizes (80 px for
   holes, 200 px for glows) and positions were chosen by the implementer, and
   80 px was chosen for holes after seeing that about 120 px gave no hit.**

## The committed frames, and how much to trust them

All eight committed cases are `screen_recording`, one frame per case directory
because each has its own radius estimate. They are screen captures of the old
UI, not camera frames: holes carry a thin dark crosshair and a dark circle drawn
around the hole under the crosshair; glows carry thick bright yellow crosshair
lines, a yellow and a red circle and a red centre marker.

**The centres were marked by eye by the implementer from screen recordings and
are low-trust.** They were read off enlarged copies of the colour frames, to the
nearest pixel, and probably carry 1-3 px of error on the holes and more on the
glows, whose shapes are irregular.

### Deliberate off-centre placement

`SimpleFinder` ranks hole candidates by distance to the frame centre. A first
cut with every target at the crop centre therefore only showed that the blob
nearest the centre is near the centre. The committed crops were re-cut so the
marked target sits off-centre by a fixed offset chosen before the finder was
run: (+9, -7), (-11, +8), (+6, +12), (-8, -10), assigned in the order of the
table below and reused across the hole and glow groups. Offsets, marks,
tolerance and finder were not tuned afterwards.

| Case | Source | Sub-crop (x, y, size) of the pane | Radius | Offset | Mark (x, y) |
|---|---|---|---|---|---|
| `hole_ac1_3` | AutoCenter1.mov t=3 s | 246,189, 80 | 11 | (+9, -7) | 49,33 |
| `hole_ac1_9` | AutoCenter1.mov t=9 s | 266,174, 80 | 10 | (-11, +8) | 29,48 |
| `hole_ac2_2` | AutoCenter2.mov t=2 s | 59,30, 80 | 11 | (+6, +12) | 46,52 |
| `hole_ac2_18` | AutoCenter2.mov t=18 s | 73,52, 80 | 11 | (-8, -10) | 32,30 |
| `glow_df0_35` | DragonFly.mov t=35 s | 323,234, 200 | 11 | (+9, -7) | 109,93 |
| `glow_df0_50` | DragonFly.mov t=50 s | 351,219, 200 | 22 | (-11, +8) | 89,108 |
| `glow_df1_30` | dragonfly1.mov t=30 s | 341,207, 200 | 13 | (+6, +12) | 106,112 |
| `glow_df1_40` | dragonfly1.mov t=40 s | 350,234, 200 | 10 | (-8, -10) | 92,90 |

All committed variants are luma (see "Channel choice"); `tolerance_px = 4`.
Three of the eight frames pass; five are `skip = true`.

### What these cases do and do not test

They test that `SimpleFinder`, given a small hand-cut region of a real frame
whose target is off-centre, returns a centre within 4 px of an eye-marked
position, with the overlays, compression artefacts and a few neighbouring holes
present. They do not test accuracy: the marks are low-trust and the tolerance is
loose. They are not raw camera frames, do not cover the real autocenter or
dragonfly region-of-interest and mask geometry, and say nothing about hole
detection with the target far from the centre. Because the sub-crops were
chosen by the implementer after a first look, they are a smoke check, not an
unbiased sample.

Frames rejected before any measurement because no target could be seen:

- `ac1_5`: the crosshair intersection falls between two holes, with no hole
  under the circle; which hole is "nearest" is a coin toss.
- `ac2_10`: the same, the circle drawn on bare tray.
- `df1_50`: no glow visible, only the overlays.

They were still run through the finder in the first round for the table.

## Results

`SimpleFinder`, `expected_radius_px` from the estimate, `mask_radius_px` = half
the crop side. "Err" is the distance in pixels from the marked centre to the
best-ranked target; "score" is `Target::score` (the saturation for glows).
Luma versus one colour channel (green for holes, blue for glows).

### Committed crops (target off-centre)

| Frame | Mode | Radius | Luma | Channel |
|---|---|---|---|---|
| ac1_3 | hole | 11 | not found | not found |
| ac1_9 | hole | 10 | found, err 3.86 (score 0.03, r 14.2) | not found |
| ac2_2 | hole | 11 | not found | found, err 2.71 (score 0.16, r 13.9) |
| ac2_18 | hole | 11 | not found | not found |
| df0_35 | glow | 11 | err 0.88, score 0.86 (r 33.9) | err 1.17, score 0.87 (r 21.6) |
| df0_50 | glow | 22 | err 2.95, score 0.83 (r 30.7) | err 4.08, score 0.87 (r 26.4) |
| df1_30 | glow | 13 | err 4.99, score 0.83 (r 19.9) | err 6.97, score 0.86 (r 18.5) |
| df1_40 | glow | 10 | err 13.02, score 0.86 (r 30.7) | err 0.98, score 0.84 (r 12.4) |

### First round: target at the crop centre (not committed)

Same frames, same sizes (holes 80 and, in brackets, the first try at 120),
glows 200. Unmarked frames show where a target was returned, with no error.

| Frame | Mode | Radius | Luma | Channel |
|---|---|---|---|---|
| ac1_3 | hole | 11 | 80: not found; 120: a neighbour, err 35.5 | 80: not found; 120: a neighbour, err 35.3 |
| ac1_5 | hole | 11 | 120: not found (unmarked) | 120: not found (unmarked) |
| ac1_9 | hole | 10 | 80: err 3.86; 120: not found | 80: not found; 120: not found |
| ac2_2 | hole | 11 | 80: err 2.41; 120: a neighbour, err 39.4 | 80: err 3.35; 120: a neighbour, err 39.5 |
| ac2_10 | hole | 11 | 120: found (96.0, 63.7), unmarked | 120: found (134.0, 70.0), unmarked |
| ac2_18 | hole | 11 | 80 and 120: not found | 80 and 120: not found |
| df0_35 | glow | 11 | err 0.88 | err 1.17 |
| df0_42 | glow | 10 | err 10.47 (r 33.3) | err 11.07 (r 16.7) |
| df0_50 | glow | 22 | err 2.95 | err 4.08 |
| df1_30 | glow | 13 | err 4.99 | err 6.97 |
| df1_40 | glow | 10 | err 11.11 (r 30.6) | err 0.98 |
| df1_50 | glow | 10 | found (436.9, 327.9), unmarked | found (437.3, 250.5), unmarked, score 0.35 |

Reading the numbers:

- Glows: a target is returned in all five marked glows (`df0_35`, `df0_42`,
  `df0_50`, `df1_30`, `df1_40`) in both variants. Within 4 px in luma: 2 of 5
  (`df0_35`, `df0_50`); in blue: 2 of 5 (`df0_35`, `df1_40`).
- Where luma fails on `df0_42` and `df1_40`, the reported radius (30.6 to 33.3
  px) is about that of the yellow overlay circle, so the glow merged with it.
  In blue, `df0_42` instead merges the faint trail with the bright core.
- Holes: one of four within 4 px in luma and one of four in green, and not the
  same frame. The off-centre re-cut changed which hole frames work (`ac2_2`
  luma was 2.41 px centred and is not found off-centre), so hit rates here are
  fragile and mostly reflect crop position. The rejection counts show mostly
  `rejected_edge` and `rejected_area`.
- The `ac1_9` hit at 3.86 px has a score of 0.03 and a radius of 14.2 against an
  expected 10: it passes narrowly and should not be read as a good detection.
- `df1_50` (no glow) still yields a target on the overlay in both variants,
  with a lower blue score.

### Channel choice

The channel variant was not clearly better than luma on the committed crops:
glows 2 of 4 marked committed frames each (luma `df0_35`, `df0_50`; blue
`df0_35`, `df1_40`), holes one each (luma `ac1_9`, green `ac2_2`). The committed
variants are therefore luma. Blue gives a more plausible radius where the
overlay circle merges with the glow, so a mono camera with no overlays should
not be taken as evidence against it.

## Known failures

Frames with `skip = true`. They stay in the repo as evidence for the spec's open
decisions; fix the finder or the frame, then remove the flag. Offsets, marks and
tolerance were not changed to make them pass.

| Frame | Measured (luma) | Best explanation |
|---|---|---|
| `hole_ac1_3` | no target | the overlay circle and crosshair drawn over the hole; the hole is half hidden. Not diagnosed beyond the rejection counts |
| `hole_ac2_2` | no target (green finds it at 2.71) | not diagnosed; the same frame was found at 2.41 px when centred, so it is position-sensitive |
| `hole_ac2_18` | no target | the overlay circle on the hole; the hole edge is also low contrast |
| `glow_df1_30` | err 4.99 px (tolerance 4) | two touching lobes; the finder returns their joint centroid, the mark is on the larger lobe, so the mark may be the wrong thing to compare against |
| `glow_df1_40` | err 13.02 px | the glow merges with the yellow overlay circle (reported radius 30.7); blue gives 0.98 px |

`df0_42` (not committed) fails in both variants: luma merges with the overlay,
blue merges the faint trail with the bright core.

## Legacy finder comparison

`LegacyFinder` (the OpenCV port of the Python pipeline, built only with
`PYCHRON_VISION_OPENCV`) and `SimpleFinder` on the same eight committed frames.
The test `LegacyFinder.RealFixtureComparison` prints this table; it runs both
finders on every frame, including those marked `skip = true`, and asserts only
that nothing crashes and results are finite. It is a report, not a gate. Neither
finder, the marks nor the tolerances were changed to produce it. Parameters are
those of the fixture tests (`expected_radius_px` from the case, mask radius half
the crop). Errors are in px against the marked centre; `ok` is within the case
`tolerance_px` (4 for every case).

| Frame | Mode | skip | SimpleFinder | LegacyFinder | Legacy, tight crop (diagnostic) |
|---|---|---|---|---|---|
| `glow_df0_35` | glow | no | 0.88 ok | 3.16 ok | - |
| `glow_df0_50` | glow | no | 2.95 ok | 3.61 ok | - |
| `glow_df1_30` | glow | yes | 4.99 | 11.66 | - |
| `glow_df1_40` | glow | yes | 13.02 | 5.66 | - |
| `hole_ac1_3` | hole | yes | none | none | 2.24 ok |
| `hole_ac1_9` | hole | no | 3.86 ok | none | 0.00 ok |
| `hole_ac2_18` | hole | yes | none | none | 1.00 ok |
| `hole_ac2_2` | hole | yes | none | none | 2.00 ok |

Summary:

- Within `tolerance_px`: `SimpleFinder` 3 of 8 (`df0_35`, `df0_50`, `ac1_9`);
  `LegacyFinder` 2 of 8 (`df0_35`, `df0_50`). They agree on the two glows that
  work and differ on the rest.
- Glows: the legacy finder returns a target on all four, at a larger error than
  `SimpleFinder` on the two that pass (3.16 and 3.61 against 0.88 and 2.95). It is
  closer on `df1_40` (5.66 against 13.02) but still outside 4 px, and further off
  on `df1_30` (11.66 against 4.99). Part of its glow error is the original's
  integer truncation of the centroid.
- Holes: the legacy finder returns nothing on all four frames. Its threshold
  limiting accepts a threshold only when 25 to 75 percent of the frame is
  foreground, which suits a tight crop (about 2.55 hole radii on a side, if the legacy
  `dim` is a radius) and not these 80 by 80 crops, where a hole of radius 11 covers
  about 6 percent.
- The last column re-runs `LegacyFinder` on a crop of `ceil(2.55 * radius)` pixels
  centred on the marked centre. It returns the hole in all four. That shows a
  tight crop centred on the mark is enough for the legacy finder to return the
  hole; it does not isolate the white-fraction limit as the only cause, because
  the tight crop also sidesteps the centre gate and most of the surrounding
  overlays. It is a diagnostic and is biased toward the mark: the crop is
  centred on it and the legacy finder prefers targets near the crop centre, so
  the small errors show detection, not accuracy. It is not counted in the totals
  above.
- Crop size caveat: this diagnostic and the legacy reading use about 2.55 hole
  radii on a side, whereas `Autocenter` crops `crop_scale * 2 * radius` (5.1
  radii at the default `crop_scale` of 2.55). Which reading of the legacy `dim`
  (radius or diameter) is right is unresolved.
- On synthetic frames the legacy and simple finders agree within 1.3 px for holes
  (on a 30 px crop, 10 stage offsets) and within 1.1 px for glows (200 px frame).
