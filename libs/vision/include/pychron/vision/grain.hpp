#pragma once

// A simulated laser-heated grain: where it lies, how hot it is and how bright
// it glows (docs/superpowers/specs/2026-10-30-dragonfly-grain-sim-design.md,
// section 3). Pure arithmetic on a clock the caller hands in: no camera, no
// threads. Every random draw is keyed by (seed, name, tick), so the same seed
// gives the same grain whatever the frame rate.

#include <cstdint>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/vision/synth.hpp"
#include "pychron/vision/types.hpp"

namespace pychron::vision {

// The model's step. Time at tick n is the state after n ticks.
inline constexpr Duration kGrainTick = std::chrono::milliseconds(10);

// Keys of a device's [<device>.sim.grain] table; a default turns its
// behaviour off. Ranges are the config reader's to check, not the model's.
struct GrainParams {
  double beam_radius_mm = 0.4;
  double reference_output_percent = 20;
  double heat_tau_s = 0.5;
  double cool_tau_s = 1.0;
  double glow_exponent = 4;
  double glow_peak = 0.6;
  int lobes = 1;
  double lobe_sigma_mm = 0.3;
  double lobe_spread_mm = 0;
  double lobe_wander_mm_per_rt_s = 0;
  double lobe_life_s = 0;
  double flicker = 0;
  double flicker_tau_s = 0.3;
  double flares_per_s = 0;
  double flare_gain = 3;
  double flare_tau_s = 0.3;
  double dropouts_per_s = 0;
  double dropout_s = 0.5;
  Vec2 creep_mm_per_s{};
  double wander_mm_per_rt_s = 0;
  double jumps_per_s = 0;
  double jump_mm = 0.3;
};

struct GrainTruth {
  Vec2 center_mm{};       // relative to the hole's center
  double heat = 0;        // 1: at reference_output_percent, beam centered
  double brightness = 0;  // peak before lobes share it
};

class GrainModel {
 public:
  // start_mm: where the grain lies, from the hole's center. A hole_radius_mm
  // of 0 or less does not clamp.
  GrainModel(GrainParams params, std::uint64_t seed, double hole_radius_mm, Vec2 start_mm);

  // Runs the whole ticks in dt (and the remainder a call before left). The
  // beam goes straight from where the last call left it to beam_mm, from the
  // hole's center, output constant. A dt of zero or less, or a beam or output
  // that is not finite, changes nothing.
  void advance(Duration dt, Vec2 beam_mm, double output_percent);

  // The lobes of light, relative to the hole's center: one a slot, their
  // peaks summing to the brightness while none is dying or rising.
  std::vector<GlowLobe> lobes() const;
  GrainTruth truth() const;
  // Ticks run so far, idle ones included.
  std::int64_t tick() const noexcept { return tick_; }

 private:
  bool idle(double output_percent) const noexcept;
  void step(Vec2 beam_mm, double output_percent);
  double brightness() const;
  double uniform(const char* key) const noexcept;
  double slot_uniform(const char* key, int slot) const noexcept;
  Vec2 draw_place(int slot) const noexcept;
  void step_lobes();

  // One slot of light. share is drawn once; fade (0 to 1) is how much of it
  // shows while the slot dies and rises again.
  struct Slot {
    Vec2 offset{};  // from the grain's center
    double share = 1;
    double fade = 1;
    enum class Phase { Alive, Dying, Rising } phase = Phase::Alive;
  };

  GrainParams params_;
  std::uint64_t seed_;
  double hole_radius_mm_;
  Vec2 center_;
  Vec2 beam_{};
  bool beam_known_ = false;
  double heat_ = 0;
  double flutter_ = 0;  // the exponent of the brightness factor
  double flare_ = 0;    // envelope, 1 at an event
  std::int64_t dark_left_ = 0;
  std::int64_t tick_ = 0;
  Duration carry_{};
  std::vector<Slot> slots_;
};

}  // namespace pychron::vision
