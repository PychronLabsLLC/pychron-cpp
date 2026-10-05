#include "pychron/devices/modbus_ids.hpp"

#include <atomic>

namespace pychron {

std::uint16_t next_modbus_transaction_id() noexcept {
  static std::atomic<std::uint32_t> next{0};
  for (;;) {
    const auto id = static_cast<std::uint16_t>(next.fetch_add(1, std::memory_order_relaxed) + 1);
    if (id != 0) return id;
  }
}

}  // namespace pychron
