#include "pychron/vision/grain.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "pychron/core/keyed_noise.hpp"

namespace pychron::vision {

namespace {

constexpr double kTickS = 0.01;
constexpr int kMaxLobes = 16;
static_assert(kGrainTick == std::chrono::milliseconds(10));
// Below this, with no output, a grain has no heat to speak of.
constexpr double kIdleBelow = 1e-6;

bool finite(Vec2 v) noexcept { return std::isfinite(v.x) && std::isfinite(v.y); }

}  // namespace

GrainModel::GrainModel(GrainParams params, std::uint64_t seed, double hole_radius_mm, Vec2 start_mm)
    : params_(params), seed_(seed), hole_radius_mm_(hole_radius_mm), center_(start_mm) {
  const int n = std::clamp(params_.lobes, 1, kMaxLobes);
  slots_.resize(static_cast<std::size_t>(n));
  double sum = 0;
  for (int i = 0; i < n; ++i) {
    Slot& k = slots_[static_cast<std::size_t>(i)];
    k.offset = draw_place(i);
    k.share = 0.5 + 0.5 * slot_uniform("grain.lobe_share", i);
    sum += k.share;
  }
  for (Slot& k : slots_) k.share /= sum;
}

// A slot's draws are keyed (tick * 64 + slot); a walk's two axes take slot
// and slot + 16, since a slot is under 16.
double GrainModel::slot_uniform(const char* key, int slot) const noexcept {
  return static_cast<double>(keyed_bits(seed_, key, tick_ * 64 + slot) >> 11) * 0x1.0p-53;
}

// A place within lobe_spread_mm of the grain's center, even over the disk:
// one draw, its high half the radius and its low half the angle.
Vec2 GrainModel::draw_place(int slot) const noexcept {
  const std::uint64_t bits = keyed_bits(seed_, "grain.lobe_place", tick_ * 64 + slot);
  const double r = params_.lobe_spread_mm * std::sqrt(static_cast<double>(bits >> 32) * 0x1.0p-32);
  const double a = 2 * std::numbers::pi * static_cast<double>(bits & 0xffffffffULL) * 0x1.0p-32;
  return {r * std::cos(a), r * std::sin(a)};
}

void GrainModel::step_lobes() {
  const GrainParams& p = params_;
  const double bound = 2 * p.lobe_spread_mm;
  const double fade_step = p.lobe_life_s > 0 ? kTickS / (p.lobe_life_s / 5) : 0;
  int i = 0;
  for (Slot& k : slots_) {
    if (p.lobe_wander_mm_per_rt_s > 0) {
      const double s = p.lobe_wander_mm_per_rt_s * std::sqrt(kTickS);
      k.offset.x += s * keyed_gauss(seed_, "grain.lobe_walk", tick_ * 64 + i);
      k.offset.y += s * keyed_gauss(seed_, "grain.lobe_walk", tick_ * 64 + i + 16);
    }
    if (p.lobe_life_s > 0) {
      switch (k.phase) {
        case Slot::Phase::Alive:
          if (slot_uniform("grain.lobe_death", i) < kTickS / p.lobe_life_s) k.phase = Slot::Phase::Dying;
          break;
        case Slot::Phase::Dying:
          k.fade -= fade_step;
          if (k.fade <= 0) {
            k.fade = 0;
            k.offset = draw_place(i);
            k.phase = Slot::Phase::Rising;
          }
          break;
        case Slot::Phase::Rising:
          k.fade += fade_step;
          if (k.fade >= 1) {
            k.fade = 1;
            k.phase = Slot::Phase::Alive;
          }
          break;
      }
    }
    // Clamped, not reflected.
    const double r = std::hypot(k.offset.x, k.offset.y);
    if (r > bound) {
      k.offset.x *= bound / r;
      k.offset.y *= bound / r;
    }
    ++i;
  }
}

// 53 bits of keyed_bits as a uniform in [0, 1); never 1, so a logarithm of
// 1 - u is finite.
double GrainModel::uniform(const char* key) const noexcept {
  return static_cast<double>(keyed_bits(seed_, key, tick_) >> 11) * 0x1.0p-53;
}

bool GrainModel::idle(double output_percent) const noexcept {
  return output_percent <= 0 && heat_ < kIdleBelow && flare_ < kIdleBelow && dark_left_ == 0;
}

void GrainModel::advance(Duration dt, Vec2 beam_mm, double output_percent) {
  if (dt <= Duration::zero() || !finite(beam_mm) || !std::isfinite(output_percent)) return;
  const double output = std::max(output_percent, 0.0);
  const Duration total = carry_ + dt;
  const std::int64_t ticks = total / kGrainTick;
  carry_ = total - ticks * kGrainTick;
  if (ticks == 0) return;
  if (!beam_known_) {
    beam_ = beam_mm;
    beam_known_ = true;
  }
  const Vec2 from = beam_;
  for (std::int64_t i = 1; i <= ticks; ++i) {
    if (idle(output)) {
      // The rest of the call changes nothing but the tick number.
      flutter_ = 0;
      flare_ = 0;
      tick_ += ticks - i + 1;
      break;
    }
    const double f = static_cast<double>(i) / static_cast<double>(ticks);
    step({from.x + (beam_mm.x - from.x) * f, from.y + (beam_mm.y - from.y) * f}, output);
  }
  beam_ = beam_mm;
}

void GrainModel::step(Vec2 beam_mm, double output_percent) {
  const GrainParams& p = params_;
  // 1, 2: overlap and heat.
  const double dx = beam_mm.x - center_.x, dy = beam_mm.y - center_.y;
  const double overlap = std::exp(-(dx * dx + dy * dy) / (2 * p.beam_radius_mm * p.beam_radius_mm));
  const double u = output_percent / p.reference_output_percent * overlap;
  const double tau = u > heat_ ? p.heat_tau_s : p.cool_tau_s;
  heat_ += (u - heat_) * (1 - std::exp(-kTickS / tau));

  // 3: motion.
  const double live = std::min(heat_, 1.0);
  center_.x += p.creep_mm_per_s.x * kTickS * live;
  center_.y += p.creep_mm_per_s.y * kTickS * live;
  if (p.wander_mm_per_rt_s > 0) {
    const double s = p.wander_mm_per_rt_s * std::sqrt(kTickS) * live;
    center_.x += s * keyed_gauss(seed_, "grain.walk_x", tick_);
    center_.y += s * keyed_gauss(seed_, "grain.walk_y", tick_);
  }
  if (p.jumps_per_s > 0 && heat_ >= 0.5 && uniform("grain.jump") < p.jumps_per_s * kTickS) {
    const double size = -p.jump_mm * std::log(1 - uniform("grain.jump_size"));
    const double angle = 2 * std::numbers::pi * uniform("grain.jump_dir");
    center_.x += size * std::cos(angle);
    center_.y += size * std::sin(angle);
  }
  if (hole_radius_mm_ > 0) {
    const double r = std::hypot(center_.x, center_.y);
    if (r > hole_radius_mm_) {
      center_.x *= hole_radius_mm_ / r;
      center_.y *= hole_radius_mm_ / r;
    }
  }

  // 4: what brightness is made of.
  if (p.flicker > 0) {
    const double a = kTickS / p.flicker_tau_s;
    flutter_ += -flutter_ * a + p.flicker * std::sqrt(2 * a) * keyed_gauss(seed_, "grain.flutter", tick_);
  }
  flare_ *= std::exp(-kTickS / p.flare_tau_s);
  if (p.flares_per_s > 0 && uniform("grain.flare") < p.flares_per_s * kTickS) flare_ = 1;
  if (dark_left_ > 0) --dark_left_;
  if (p.dropouts_per_s > 0 && uniform("grain.dropout") < p.dropouts_per_s * kTickS)
    dark_left_ = std::max<std::int64_t>(1, std::llround(p.dropout_s / kTickS));

  // 5: lobes. Their state moves only on a tick that runs, so a skipped idle
  // span leaves them as they were.
  step_lobes();

  ++tick_;
}

double GrainModel::brightness() const {
  const GrainParams& p = params_;
  const double flare = 1 + (p.flare_gain - 1) * flare_;
  return dark_left_ > 0 ? 0.0 : p.glow_peak * std::pow(heat_, p.glow_exponent) * std::exp(flutter_) * flare;
}

GrainTruth GrainModel::truth() const { return {center_, heat_, brightness()}; }

std::vector<GlowLobe> GrainModel::lobes() const {
  const double b = brightness();
  std::vector<GlowLobe> out;
  out.reserve(slots_.size());
  for (const Slot& k : slots_)
    out.push_back({{center_.x + k.offset.x, center_.y + k.offset.y}, params_.lobe_sigma_mm, b * k.share * k.fade});
  return out;
}

}  // namespace pychron::vision
