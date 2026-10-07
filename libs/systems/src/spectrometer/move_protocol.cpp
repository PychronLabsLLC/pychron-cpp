#include "pychron/systems/spectrometer/move_protocol.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace pychron::spectrometer {

std::vector<double> af_demag_trajectory(double from, double target, const AfDemagSettings& af, Limits limits) {
  std::vector<double> out;
  if (!af.enabled || std::abs(target - from) < af.threshold) return out;
  if (af.period <= Duration::zero() || af.duration <= Duration::zero()) return out;
  const Duration dt = af.period / kAfDemagStepsPerPeriod;
  if (dt <= Duration::zero()) return out;
  const double period_s = std::chrono::duration<double>(af.period).count();
  const double duration_s = std::chrono::duration<double>(af.duration).count();
  const auto n = af.duration / dt;
  for (std::int64_t i = 0; i < n; ++i) {
    const double t = std::chrono::duration<double>(dt * i).count();
    const double amplitude = af.start_amplitude * (1.0 - t / duration_s);
    const double value = target + amplitude * std::sin(2.0 * std::numbers::pi * t / period_s);
    out.push_back(limits.valid() ? limits.clamp(value) : value);
  }
  return out;
}

void sleep_on(const Clock& clock, Duration d) { clock.sleep_for(d); }

namespace {

void keep_first(std::optional<Error>& first, const Result<void>& r) {
  if (!r && !first) first = r.error();
}

}  // namespace

Result<MoveOutcome> execute_move(const MovePlan& plan, const MoveDeps& deps) {
  const TimePoint start = deps.clock.now();
  auto sleep = [&](Duration d) {
    if (deps.sleep) {
      deps.sleep(d);
    } else {
      sleep_on(deps.clock, d);
    }
  };

  MoveOutcome out;
  out.from = plan.from;
  out.to = plan.to;
  std::optional<Error> first;
  std::vector<ChannelId> attempted;  // protect() issued, successful or not
  bool blank_attempted = false;
  bool set_issued = false;  // a positioner.set() went out, successful or not

  // 1. protect, then blank.
  if (!plan.protect.empty() && deps.control == nullptr) {
    first = Error{ErrorKind::Config, "move needs detector protection but no detector_control is bound", "magnet"};
  }
  for (const auto& ch : plan.protect) {
    if (first) break;
    attempted.push_back(ch);
    auto r = deps.control->protect(ch, true);
    keep_first(first, r);
    if (r) {
      out.protected_channels.push_back(ch);
      if (deps.on_protect) deps.on_protect(ch, true);
    }
  }
  if (!first && plan.blank && deps.beam_blank != nullptr) {
    blank_attempted = true;
    auto r = deps.beam_blank->blank(true);
    keep_first(first, r);
    out.blanked = r.has_value();
  }

  // 2. AF demag, 3. set + wait.
  if (!first) {
    out.demag = af_demag_trajectory(plan.from, plan.to, plan.af_demag,
                                    plan.limits.valid() ? plan.limits : deps.positioner.limits());
    const Duration dt = plan.af_demag.period / kAfDemagStepsPerPeriod;
    for (double v : out.demag) {
      set_issued = true;
      auto r = deps.positioner.set(v);
      keep_first(first, r);
      if (first) break;
      sleep(dt);
    }
  }
  if (!first) {
    set_issued = true;
    keep_first(first, deps.positioner.set(plan.to));
  }
  if (!first) {
    bool reported_motion = false;
    if (plan.wait_moving) {
      const TimePoint wait_start = deps.clock.now();
      for (;;) {
        auto moving = deps.positioner.moving();
        if (!moving) {
          first = moving.error();
          break;
        }
        if (!*moving) break;
        reported_motion = true;
        if (deps.clock.now() - wait_start >= plan.max_wait) {
          first = Error{ErrorKind::Timeout, "magnet still moving after max_wait", "magnet"};
          break;
        }
        sleep(plan.poll_interval);
      }
    }
    if (!first && !reported_motion && std::abs(plan.to - plan.from) >= plan.epsilon) sleep(plan.settle);
  }

  // A failure once a set() was issued does not mean the magnet is still: a
  // set() whose reply was lost was delivered all the same. Give it the settle
  // time before the beam and the detectors are exposed again: never less than
  // `failure_settle`, whatever the caller chose for a successful move.
  if (first && set_issued) sleep(std::max(plan.settle, plan.failure_settle));

  // 4. cleanup in reverse order, always.
  if (blank_attempted) keep_first(first, deps.beam_blank->blank(false));
  for (auto it = attempted.rbegin(); it != attempted.rend(); ++it) {
    auto r = deps.control->protect(*it, false);
    keep_first(first, r);
    if (r && deps.on_protect) deps.on_protect(*it, false);
  }

  out.elapsed = deps.clock.now() - start;
  if (first) return fail(*first);
  return out;
}

}  // namespace pychron::spectrometer
