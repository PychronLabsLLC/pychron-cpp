# Follow XY Plot, Cold Grain, Simulation Reset Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the stage strips with a 2D plot and fence, show the cold grain in its hole, add a simulation reset, and stop the camera pane resizing.

**Architecture:** `DragonflyStep` carries the fence. `SimTrayCamera` holds one grain per hole and draws them while the beam is off. `LaserBridge::reset_simulation` runs on the worker. `FollowPlotView` gains a second `QCustomPlot`.

**Tech Stack:** C++20, Qt6, QCustomPlot, GoogleTest, QtTest.

**Spec:** `docs/superpowers/specs/2026-10-31-follow-xy-reset-design.md`

## Global Constraints

- Colours of live plots come from `plot_style()` / `plot_series_color(slot)`.
- With no `[sim.grain]` table frames stay byte for byte as they are.
- The hole finder is not changed.
- Nothing is reset, and no button is offered, on a real laser.
- Camera access stays under the `LaserSystem` gate.

## Review Focus

- Autocenter on a hole holding a sized grain still centers the hole (test in Task 2).
- Coming back to a hole after a long absence: the idle run does not cost real time or move the grain (test in Task 2).
- A reset straight after a run: late samples do not refill cleared plots (test in Task 3).
- A NaN grain row breaks the track instead of drawing a line through (0, 0) (test in Task 4).
- Tab change with a wrapped caution on the Calibration page: nothing clipped, camera the same size (test in Task 5).

---

### Task 1: The step carries the fence

**Files:**
- Modify: `libs/vision/include/pychron/vision/dragonfly.hpp`, `libs/vision/src/dragonfly.cpp`
- Modify: `apps/pychron-ui/src/follow_plot_model.hpp`, `apps/pychron-ui/src/follow_plot_model.cpp`
- Test: `tests/vision/test_dragonfly.cpp`, `tests/ui/test_follow_plot_model.cpp`

**Interfaces:**
- Produces: `DragonflyStep::start_mm`, `DragonflyStep::perimeter_mm`; `struct FollowFence { double x, y, radius_mm; }`; `std::optional<FollowFence> FollowPlotModel::fence() const`.

- [ ] Write failing tests: step on Track, Invalid and Elapsed reports `start_mm` and `perimeter_mm`; model's `fence()` is empty before a row, latest wins, `clear()` empties it, `perimeter_mm == 0` does not set it.
- [ ] Set both fields where `out.stage_mm` is set; add `fence_` to the model.
- [ ] Run `pychron_vision_tests --gtest_filter='Dragonfly*'` and `pychron_ui_test_follow_plot_model`: pass.
- [ ] Commit `feat(vision): a dragonfly step says where its fence is`.

### Task 2: One grain a hole, drawn cold

**Files:**
- Modify: `libs/vision/include/pychron/vision/synth.hpp`, `libs/vision/src/synth.cpp`
- Modify: `libs/laser/include/pychron/laser/tray_camera.hpp`, `libs/laser/src/tray_camera.cpp`
- Test: `tests/vision/test_synth.cpp`, `tests/laser/test_tray_camera.cpp`, `tests/laser/test_laser_autocenter.cpp`

**Interfaces:**
- Produces: `vision::draw_grain(Frame&, const HoleScene&, std::span<const Vec2>, Vec2, double level = 0.5)`; `SimTrayCamera::reset_grains()`.

- [ ] Write failing tests from spec section 7 for `draw_grain`, the camera and autocenter.
- [ ] Implement `draw_grain` (point-in-polygon over the outline's bounding box, 1 px soft edge from distance to nearest segment).
- [ ] Replace `grain_`, `grain_hole_`, `grain_at_` by `std::map<std::size_t, HeldGrain> grains_`; `advance_grain` runs the absent time at output 0 before the beam resumes; `grab()` draws each in-view hole's outline with the beam off; add `reset_grains()`.
- [ ] Rename `ANewHoleIsANewGrain`'s claim if it asserts a fresh grain on return; the spec departs from it.
- [ ] Run `pychron_vision_tests`, `pychron_laser_tests`: pass. If autocenter fails, change `level`, and record the value in spec section 4.
- [ ] Commit `feat(laser): a simulated grain lies in its hole with the beam off`.

### Task 3: Reset

**Files:**
- Modify: `libs/laser/include/pychron/laser/laser_system.hpp`, `libs/laser/src/laser_system.cpp`
- Modify: `apps/pychron-ui/src/laser_bridge.hpp`, `apps/pychron-ui/src/laser_bridge.cpp`, `apps/pychron-ui/src/laser_window.hpp`, `apps/pychron-ui/src/laser_window.cpp`
- Test: `tests/laser/test_tray_camera.cpp` or `test_laser_system`, `tests/ui/test_laser_bridge.cpp`, `tests/ui/test_laser_window.cpp`

**Interfaces:**
- Consumes: `SimTrayCamera::reset_grains()`.
- Produces: `LaserSystem::reset_simulated_grains()`, `LaserBridge::reset_simulation(bool center)`, button `pattern_reset_sim`.

- [ ] Write failing tests: bridge refuses off sim; grain back and stage on last hole; window button absent on real laser, clears plots on success; a tray change resets, a re-read of the same tray does not.
- [ ] Implement as spec section 5; `commandFinished("reset_simulation")` success clears `follow_model_` and refreshes the view.
- [ ] Run `pychron_ui_test_laser_bridge`, `pychron_ui_test_laser_window`: pass.
- [ ] Commit `feat(ui): reset a simulated tray from the laser window`.

### Task 4: The XY plot

**Files:**
- Modify: `apps/pychron-ui/src/follow_plot_view.hpp`, `apps/pychron-ui/src/follow_plot_view.cpp`
- Test: `tests/ui/test_laser_views.cpp`, `tests/ui/test_laser_window.cpp`
- Docs: `docs/user/05-laser.md`

**Interfaces:**
- Consumes: `FollowPlotModel::fence()`.
- Produces: `FollowPlotView::xy_plot()`, `xy_point_count(FollowLine stage_or_grain_x)`, `fence_point_count()`, `kStrips = 3`.

- [ ] Write failing tests from spec section 7; rename `follow_plot_has_five_strips` to three.
- [ ] Build the XY plot as spec section 3; strips use lines 4 to 9 (`(k - 4) / 2`).
- [ ] Document the plot, the cold grain and Reset simulation in `docs/user/05-laser.md`.
- [ ] Run `pychron_ui_test_laser_views`, `pychron_ui_test_laser_window`: pass.
- [ ] Commit `feat(ui): follow plots show the stage and grain in a plane, with the fence`.

### Task 5: Camera pane

**Files:**
- Modify: `apps/pychron-ui/src/laser_window.cpp`
- Test: `tests/ui/test_laser_window.cpp`

- [ ] Write failing test: window shown offscreen at 1200 x 800, `camera` size read on each tab, all equal, also with a caution set on the Calibration page.
- [ ] Find the cause from what the test shows; fix in `LaserWindow::build`.
- [ ] Run `pychron_ui_test_laser_window`: pass.
- [ ] Commit `fix(ui): the camera keeps its size when the tab changes`.

### Task 6: Landing

- [ ] `python3 tools/quality_check.py`, `python3 tools/compiler_sweep.py --build-dir build/dev-ui`, `ctest --preset dev-ui`.
- [ ] Review the branch on Sonnet; fix.
- [ ] Rebase on `origin/develop`, `git push origin feat/follow-xy-reset:develop`, remove the worktree and branch.

## Open questions

None.
