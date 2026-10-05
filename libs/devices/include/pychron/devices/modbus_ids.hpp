#pragma once

// Modbus TCP transaction ids for drivers. Several drivers can share one
// connection (valves, gauges and a heater on one PLC); if each counted its
// own ids, a late reply to one driver's request could carry the id another
// driver is waiting for and be taken as its reply. One process-wide counter
// keeps every id on every connection distinct until it wraps after 65536
// requests, long after any late reply has been discarded.

#include <cstdint>

namespace pychron {

// Thread-safe. Never 0, so a zeroed frame never matches.
std::uint16_t next_modbus_transaction_id() noexcept;

}  // namespace pychron
