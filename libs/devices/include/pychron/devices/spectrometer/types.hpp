#pragma once

// Value types shared by the spectrometer role interfaces (roles.hpp) and the
// systems layer built on them. Plain values: they cross thread boundaries by
// copy.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pychron/core/clock.hpp"

namespace pychron::spectrometer {

// Acquirer channel as named by its driver ("H1", "counter:0"); config binds
// detectors to channels.
using ChannelId = std::string;

// Detector name from config ("H1", "CDD").
using DetectorId = std::string;

// Closed interval [min, max] in the owner's native units.
struct Range {
  double min = 0.0;
  double max = 0.0;

  bool valid() const noexcept { return min <= max; }
  bool contains(double v) const noexcept { return v >= min && v <= max; }
  double clamp(double v) const noexcept { return v < min ? min : (v > max ? max : v); }

  friend bool operator==(const Range&, const Range&) = default;
};

// Travel limits of an IMassPositioner in its native axis.
using Limits = Range;

// One acquirer read. `values` are raw units (V, fA, counts).
struct Frame {
  TimePoint ts{};                                    // instrument time if provided, else host monotonic
  std::uint64_t seq = 0;                             // acquirer-local; gaps = dropped frames
  std::vector<std::pair<ChannelId, double>> values;
  bool integrated = false;                           // vendor already averaged over `span`
  Duration span{};                                   // integration period or sample period

  // Value for `channel`, or nullopt if this frame does not carry it.
  std::optional<double> value(const ChannelId& channel) const;

  friend bool operator==(const Frame&, const Frame&) = default;
};

// A source parameter as the driver reports it. Consumers never assume
// `actual` exists.
struct Readback {
  double setpoint = 0.0;
  std::optional<double> actual;

  friend bool operator==(const Readback&, const Readback&) = default;
};

// IDetectorControl capabilities.
enum class DetectorCap : std::uint8_t {
  Gain = 1U << 0,
  Deflection = 1U << 1,
  Protect = 1U << 2,
  CddVoltage = 1U << 3,
};

class Caps {
 public:
  constexpr Caps() = default;
  constexpr Caps(DetectorCap cap) : bits_(static_cast<std::uint8_t>(cap)) {}  // NOLINT(google-explicit-constructor)

  constexpr bool has(DetectorCap cap) const noexcept {
    return (bits_ & static_cast<std::uint8_t>(cap)) != 0;
  }
  constexpr bool empty() const noexcept { return bits_ == 0; }
  constexpr std::uint8_t bits() const noexcept { return bits_; }

  friend constexpr Caps operator|(Caps a, Caps b) noexcept {
    Caps c;
    c.bits_ = static_cast<std::uint8_t>(a.bits_ | b.bits_);
    return c;
  }
  friend constexpr Caps operator&(Caps a, Caps b) noexcept {
    Caps c;
    c.bits_ = static_cast<std::uint8_t>(a.bits_ & b.bits_);
    return c;
  }
  friend constexpr bool operator==(Caps, Caps) = default;

 private:
  std::uint8_t bits_ = 0;
};

constexpr Caps operator|(DetectorCap a, DetectorCap b) noexcept { return Caps(a) | Caps(b); }

// "gain", "deflection", "protect", "cdd_voltage".
std::string to_string(DetectorCap cap);

// "deflection|protect"; "" when empty.
std::string to_string(Caps caps);

}  // namespace pychron::spectrometer
