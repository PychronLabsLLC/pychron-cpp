#pragma once

// Value types shared by drivers and managers. Sample and ValveState are the
// same types the Scheduler and SignalBus carry (pychron/core/events.hpp), so
// driver output crosses thread boundaries without conversion.

#include <cstdint>
#include <string>
#include <string_view>

#include "pychron/core/error.hpp"
#include "pychron/core/events.hpp"

namespace pychron {

std::string_view to_string(ValveState state) noexcept;

// Where a valve lives on its actuator, verbatim from config (`address = "1"`).
// Its meaning (relay index, bank/bit, register) belongs to the driver.
struct ValveAddress {
  std::string value;

  // `value` as a non-negative decimal index, surrounding spaces allowed;
  // Config error otherwise.
  Result<std::int64_t> as_index() const;

  friend bool operator==(const ValveAddress&, const ValveAddress&) = default;
  friend bool operator<(const ValveAddress& a, const ValveAddress& b) { return a.value < b.value; }
};

}  // namespace pychron
