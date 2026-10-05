#include "pychron/devices/modbus_ids.hpp"

#include <gtest/gtest.h>

#include <mutex>
#include <set>
#include <thread>
#include <vector>

using namespace pychron;

TEST(ModbusIds, DriversOnOneConnectionNeverShareAnId) {
  // Four drivers taking ids at once, as valves, gauges and a heater on one
  // PLC would: no id is handed out twice.
  std::mutex m;
  std::set<std::uint16_t> seen;
  std::size_t taken = 0;
  std::vector<std::thread> drivers;
  for (int d = 0; d < 4; ++d) {
    drivers.emplace_back([&] {
      for (int i = 0; i < 5000; ++i) {
        const auto id = next_modbus_transaction_id();
        std::lock_guard lock(m);
        seen.insert(id);
        ++taken;
      }
    });
  }
  for (auto& t : drivers) t.join();
  EXPECT_EQ(seen.size(), taken);
  EXPECT_FALSE(seen.contains(0));
}

TEST(ModbusIds, WrapsWithoutZero) {
  for (int i = 0; i < 70000; ++i) ASSERT_NE(next_modbus_transaction_id(), 0);
}
