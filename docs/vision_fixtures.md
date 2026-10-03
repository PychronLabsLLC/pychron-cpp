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

```
ffmpeg -ss <t> -i in.mov -frames:v 1 -vf "crop=w:h:x:y,format=gray" out.pgm
```

Use `extractplanes=g` (or `r`, `b`) in place of `format=gray` for one colour
channel. `ffmpeg` is for future use; the committed frames were cut differently.

## The committed frames, and how much to trust them

All eight committed cases are `screen_recording`. They were cut from the user's
recordings (`AutoCenter1.mov`, `AutoCenter2.mov`, `DragonFly.mov`,
`dragonfly1.mov`, Drive folder `0.1_Pychron/PychronVideos`) by the controller,
who cropped the video pane; the exact time of each extract is not recorded (the
names `ac1_3`, `df0_35` ... are extract labels). Each is one frame per case
directory because each has its own radius estimate.

**The centres were marked by eye by the implementer from screen recordings and
are low-trust.** They were read off enlarged copies of the colour frames, to the
nearest pixel, and probably carry 1-3 px of error on the holes and more on the
glows, whose shapes are irregular. The recordings are screen captures of the old
UI, not camera frames: holes carry a thin dark crosshair and a dark circle
drawn around the hole under the crosshair; glows carry thick bright yellow
crosshair lines, a yellow and a red circle and a red centre marker.

Cases:

| Case | Source | Crop (x, y, size) | Radius | Mark |
|---|---|---|---|---|
| `hole_ac1_3` | AutoCenter1.mov, `ac1_3` | 255,182, 80 | 11 | 40,40 |
| `hole_ac1_9` | AutoCenter1.mov, `ac1_9` | 255,182, 80 | 10 | 40,40 |
| `hole_ac2_2` | AutoCenter2.mov, `ac2_2` | 65,42, 80 | 11 | 40,40 |
| `hole_ac2_18` | AutoCenter2.mov, `ac2_18` | 65,42, 80 | 11 | 40,40 |
| `glow_df0_35` | DragonFly.mov, `df0_35` | 332,227, 200 | 11 | 100,100 |
| `glow_df0_50` | DragonFly.mov, `df0_50` | 340,227, 200 | 22 | 100,100 |
| `glow_df1_30` | dragonfly1.mov, `df1_30` | 347,219, 200 | 13 | 100,100 |
| `glow_df1_40` | dragonfly1.mov, `df1_40` | 342,224, 200 | 10 | 100,100 |

All committed variants are luma (see "Channel choice"). `tolerance_px = 4`.
Four of the eight frames pass; four are `skip = true` (Known failures).

Frames rejected before any measurement because no target could be seen:

- `ac1_5`: the crosshair intersection falls between two holes, with no hole
  under the circle; which hole is "nearest" is a coin toss.
- `ac2_10`: the same, the circle drawn on bare tray.
- `df1_50`: no glow visible, only the overlays.

They were still run through the finder for the table; no error is computed.

## Results

`SimpleFinder`, `expected_radius_px` from the estimate, `mask_radius_px` = half
the crop side. "Err" is the distance in pixels from the marked centre to the
best-ranked target; "sat" is `Target::score` (the saturation for glows). The
chosen crop is a free choice of the caller, so hole frames are shown at two
sizes: 120 (the first try) and 80 (committed).

Luma / one colour channel (green for holes, blue for glows):

| Frame | Mode | Radius | Crop | Luma | Channel |
|---|---|---|---|---|---|
| ac1_3 | hole | 11 | 120 | found a neighbour hole, err 35.5 | found a neighbour hole, err 35.3 |
| ac1_3 | hole | 11 | 80 | not found | not found |
| ac1_5 | hole | 11 | 120 | not found (unmarked) | not found (unmarked) |
| ac1_9 | hole | 10 | 120 | not found | not found |
| ac1_9 | hole | 10 | 80 | found, err 3.86 | not found |
| ac2_2 | hole | 11 | 120 | found a neighbour hole, err 39.4 | found a neighbour hole, err 39.5 |
| ac2_2 | hole | 11 | 80 | found, err 2.41 | found, err 3.35 |
| ac2_10 | hole | 11 | 120 | found (96.0, 63.7), unmarked | found (134.0, 70.0), unmarked |
| ac2_18 | hole | 11 | 120 | not found | not found |
| ac2_18 | hole | 11 | 80 | not found | not found |
| df0_35 | glow | 11 | 200 | found, err 0.88, sat 0.86 | found, err 1.17, sat 0.87 |
| df0_42 | glow | 10 | 200 | found, err 10.47, sat 0.82 | found, err 11.07, sat 0.82 |
| df0_50 | glow | 22 | 200 | found, err 2.95, sat 0.83 | found, err 4.08, sat 0.87 |
| df1_30 | glow | 13 | 200 | found, err 4.99, sat 0.83 | found, err 6.97, sat 0.86 |
| df1_40 | glow | 10 | 200 | found, err 11.11, sat 0.86 | found, err 0.98, sat 0.84 |
| df1_50 | glow | 10 | 200 | found (436.9, 327.9), unmarked, sat 0.80 | found (437.3, 250.5), unmarked, sat 0.35 |

Reading the table:

- Glows are mostly findable. Luma and blue agree within a pixel on `df0_35`;
  they differ where an overlay merges with the glow (`df0_42`, `df1_40` luma:
  the reported radius of 31-33 px is the yellow overlay circle, not the glow).
- The hole finder is fragile on these frames. At crop 120 it either finds a
  neighbouring hole (`ac1_3`, `ac2_2`: 35-40 px away) or nothing. At 80 it
  finds two of four. The rejection counts show mostly `rejected_edge` and
  `rejected_area`, i.e. components touching the mask or of the wrong size.
- The `ac1_9` hit at 3.86 px has a `score` of 0.03 and a radius of 14.2 against
  an expected 10: it passes the tolerance narrowly and should not be read as a
  good detection.
- `df1_50` (no glow) still yields a target on the overlay in both variants,
  with a lower blue score; nothing in the finder tells that apart from a glow
  except `score`.

### Channel choice

The channel variant was not clearly better than luma: both put two of five
marked glow frames in tolerance (luma `df0_35`, `df0_50`; blue `df0_35`,
`df1_40`), and for holes luma found one and green none at crop 80. The
committed variants are therefore luma. Blue gives a more plausible radius where
the overlay circle merges with the glow, so a mono camera with no overlays
should not be taken as evidence against it.

## Known failures

Frames with `skip = true`. They stay in the repo as evidence for the spec's open
decisions; fix the finder or the frame, then remove the flag. The marks and
tolerance were not changed to make them pass.

| Frame | Measured | Best explanation |
|---|---|---|
| `hole_ac1_3` | no target at crop 80; a neighbour 35.5 px away at crop 120 | the overlay circle and crosshair drawn over the hole; the hole itself is half hidden. Not diagnosed beyond the rejection counts |
| `hole_ac2_18` | no target at crops 80 and 120 | the same overlay circle on the hole; the hole edge is also low contrast |
| `glow_df1_30` | err 4.99 px (tolerance 4) | two touching lobes; the finder returns their joint centroid, the mark is on the larger lobe. The mark may be the wrong thing to compare against |
| `glow_df1_40` | err 11.11 px | in luma the glow merges with the yellow overlay circle (reported radius 30.6); the blue channel gives 0.98 px |

`df0_42` (not committed) fails in both variants: luma merges with the overlay,
blue merges the faint trail with the bright core.
