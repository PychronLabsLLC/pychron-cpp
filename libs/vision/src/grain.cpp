#include "pychron/vision/grain.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
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

// Distance from a point (relative to the grain's center) to the outline: 0
// inside, else to the nearest edge. An empty outline is a point at the
// center, so the distance is the point's length.
double gap_to_outline(const std::vector<Vec2>& outline, Vec2 from_center) {
  if (outline.empty()) return std::hypot(from_center.x, from_center.y);
  bool inside = false;
  double best = std::numeric_limits<double>::infinity();
  const std::size_t n = outline.size();
  for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
    const Vec2 a = outline[j], b = outline[i];
    if ((a.y > from_center.y) != (b.y > from_center.y) &&
        from_center.x < (b.x - a.x) * (from_center.y - a.y) / (b.y - a.y) + a.x)
      inside = !inside;
    const double ex = b.x - a.x, ey = b.y - a.y;
    const double len2 = ex * ex + ey * ey;
    double t = len2 > 0 ? ((from_center.x - a.x) * ex + (from_center.y - a.y) * ey) / len2 : 0;
    t = std::clamp(t, 0.0, 1.0);
    best = std::min(best, std::hypot(from_center.x - (a.x + t * ex), from_center.y - (a.y + t * ey)));
  }
  return inside ? 0.0 : best;
}

}  // namespace

GrainModel::GrainModel(GrainParams params, std::uint64_t seed, double hole_radius_mm, Vec2 start_mm)
    : params_(params), seed_(seed), hole_radius_mm_(hole_radius_mm), center_(start_mm) {
  if (params_.grain_radius_mm > 0) {
    radius_ = params_.grain_radius_mm;
    const int sides = std::clamp(params_.grain_sides, 3, 16);
    std::vector<Vec2> corners;
    for (int i = 0; i < sides; ++i) {
      const double u = slot_uniform("grain.corner_angle", i);
      const double v = slot_uniform("grain.corner_radius", i);
      const double a = 2 * std::numbers::pi * (i + 0.5 * (u - 0.5)) / sides;
      const double r = 1 + params_.grain_irregularity * (2 * v - 1);
      corners.push_back({r * std::cos(a), r * std::sin(a)});
    }
    constexpr int kPerEdge = 8;
    for (int i = 0; i < sides; ++i) {
      const Vec2 a = corners[static_cast<std::size_t>(i)], b = corners[static_cast<std::size_t>((i + 1) % sides)];
      for (int j = 0; j < kPerEdge; ++j) {
        const double f = static_cast<double>(j) / kPerEdge;
        unit_.push_back({a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f});
      }
    }
    double twice_area = 0;
    for (std::size_t i = 0; i < unit_.size(); ++i) {
      const Vec2 a = unit_[i], b = unit_[(i + 1) % unit_.size()];
      twice_area += a.x * b.y - b.x * a.y;
    }
    const double scale = std::sqrt(std::numbers::pi / (std::abs(twice_area) / 2));
    for (Vec2& p : unit_) p = {p.x * scale, p.y * scale};
    reshape();
  }
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

void GrainModel::reshape() {
  shape_.clear();
  reach_ = 0;
  if (radius_ <= 0) return;
  const double m = roundness_;
  shape_.reserve(unit_.size());
  for (const Vec2 p : unit_) {
    const double len = std::hypot(p.x, p.y);
    const double k = (1 - m) + (len > 0 ? m / len : 0);
    const Vec2 q{radius_ * k * p.x, radius_ * k * p.y};
    shape_.push_back(q);
    reach_ = std::max(reach_, std::hypot(q.x, q.y));
  }
}

// A slot's draws are keyed (tick * 64 + slot); a walk's two axes take slot
// and slot + 16, since a slot is under 16.
double GrainModel::slot_uniform(const char* key, int slot) const noexcept {
  return static_cast<double>(keyed_bits(seed_, key, tick_ * 64 + slot) >> 11) * 0x1.0p-53;
}

// A place within lobe_spread_mm (the radius, for a sized grain) of the grain's center, even over the disk:
// one draw, its high half the radius and its low half the angle.
Vec2 GrainModel::draw_place(int slot) const noexcept {
  const std::uint64_t bits = keyed_bits(seed_, "grain.lobe_place", tick_ * 64 + slot);
  const double spread = radius_ > 0 ? radius_ : params_.lobe_spread_mm;
  const double r = spread * std::sqrt(static_cast<double>(bits >> 32) * 0x1.0p-32);
  const double a = 2 * std::numbers::pi * static_cast<double>(bits & 0xffffffffULL) * 0x1.0p-32;
  return {r * std::cos(a), r * std::sin(a)};
}

void GrainModel::step_lobes() {
  const GrainParams& p = params_;
  const double bound = radius_ > 0 ? radius_ : 2 * p.lobe_spread_mm;
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
  double d2 = dx * dx + dy * dy;
  if (!shape_.empty()) {
    const double gap = gap_to_outline(shape_, {dx, dy});
    d2 = gap * gap;
  }
  const double overlap = std::exp(-d2 / (2 * p.beam_radius_mm * p.beam_radius_mm));
  const double u = output_percent / p.reference_output_percent * overlap;
  const double tau = u > heat_ ? p.heat_tau_s : p.cool_tau_s;
  heat_ += (u - heat_) * (1 - std::exp(-kTickS / tau));

  // 3: motion.
  const double live = std::min(heat_, 1.0);
  center_.x += p.creep_mm_per_s.x * kTickS * live;
  center_.y += p.creep_mm_per_s.y * kTickS * live;
  // Melting: radius only falls, and the grain rounds as it goes. It draws
  // nothing, so no other keyed draw moves.
  if (p.melt_mm_per_s > 0 && radius_ > p.min_radius_mm) {
    radius_ = std::max(radius_ - p.melt_mm_per_s * kTickS * live, p.min_radius_mm);
    roundness_ = std::clamp((p.grain_radius_mm - radius_) / (p.grain_radius_mm - p.min_radius_mm), 0.0, 1.0);
    reshape();
  }
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
    const double bound = std::max(hole_radius_mm_ - reach_, 0.0);
    const double r = std::hypot(center_.x, center_.y);
    if (r > bound) {
      center_.x *= bound / r;
      center_.y *= bound / r;
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

GrainTruth GrainModel::truth() const { return {center_, heat_, brightness(), radius_}; }

std::vector<GlowLobe> GrainModel::lobes() const {
  const bool sized = radius_ > 0;
  const double b = brightness() * (sized ? 1 - params_.body_glow : 1.0);
  const double sigma = sized ? std::min(params_.lobe_sigma_mm, radius_) : params_.lobe_sigma_mm;
  std::vector<GlowLobe> out;
  out.reserve(slots_.size());
  for (const Slot& k : slots_) out.push_back({{center_.x + k.offset.x, center_.y + k.offset.y}, sigma, b * k.share * k.fade});
  return out;
}

std::optional<GlowBody> GrainModel::body() const {
  if (shape_.empty()) return std::nullopt;
  GlowBody out;
  out.outline_mm.reserve(shape_.size());
  for (const Vec2 p : shape_) out.outline_mm.push_back({center_.x + p.x, center_.y + p.y});
  out.peak = params_.body_glow * brightness();
  return out;
}

}  // namespace pychron::vision
