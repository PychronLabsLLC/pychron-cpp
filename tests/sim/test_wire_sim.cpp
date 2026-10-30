// SimSystem::wire_for_transport: each emulated kind names itself and cuts
// the bytes its own driver writes into requests (sim serve spec 4.1).
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "pychron/codecs/lakeshore.hpp"
#include "pychron/codecs/maxigauge.hpp"
#include "pychron/codecs/modbus.hpp"
#include "pychron/codecs/proxr.hpp"
#include "pychron/core/clock.hpp"
#include "pychron/core/config/system_config.hpp"
#include "pychron/sim/sim_system.hpp"
#include "pychron/sim/wire_sim.hpp"

namespace {

using namespace pychron;
using sim::SimSystem;

config::SystemConfig one_driver(const std::string& kind, toml::table options = {}) {
  config::SystemConfig c;
  config::DriverConfig d;
  d.name = "dev";
  d.kind = kind;
  d.transport = "wire";
  d.options = std::move(options);
  c.drivers.emplace(d.name, std::move(d));
  return c;
}

// Cuts `stream` into requests with `request`; what is left incomplete is dropped.
std::vector<std::size_t> cut(const sim::RequestFramer& request, Bytes stream) {
  std::vector<std::size_t> lengths;
  while (auto n = request(stream)) {
    if (*n == 0) break;
    lengths.push_back(*n);
    stream.erase(stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(*n));
  }
  return lengths;
}

Bytes joined(std::initializer_list<Bytes> parts) {
  Bytes out;
  for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
  return out;
}

}  // namespace

TEST(WireSim, EveryEmulatedKindNamesItself) {
  for (const char* kind :
       {"proxr_relay", "agilent_switch", "qtegra_valves", "qtegra_gauges", "pychron_valves", "ngx_valves",
        "pfeiffer_maxigauge", "varian_xgs600", "gp_microion", "plc2000_valves", "plc2000_gauges", "lakeshore",
        "chromium"}) {
    ManualClock clock;
    SimSystem sim(clock, {});
    const auto cfg = one_driver(kind);
    const auto wire = sim.wire_for_transport("wire", cfg);
    EXPECT_EQ(wire.emulator, kind);
    EXPECT_TRUE(wire.hook) << kind;
    EXPECT_TRUE(wire.request) << kind;
  }
}

TEST(WireSim, UnknownKindIsSilentWire) {
  ManualClock clock;
  SimSystem sim(clock, {});
  const auto cfg = one_driver("adc_bank");
  const auto wire = sim.wire_for_transport("wire", cfg);
  EXPECT_EQ(wire.emulator, "");
  ASSERT_TRUE(wire.hook);
  EXPECT_TRUE(wire.hook(to_bytes("anything\r")).empty());
  ASSERT_TRUE(wire.request);
}

TEST(WireSim, NoDriverIsEmpty) {
  ManualClock clock;
  SimSystem sim(clock, {});
  const auto cfg = one_driver("proxr_relay");
  const auto wire = sim.wire_for_transport("other", cfg);
  EXPECT_EQ(wire.emulator, "");
  EXPECT_FALSE(wire.hook);
  EXPECT_FALSE(sim.hook_for_transport("other", cfg));
}

TEST(WireSim, FramerMatchesDriverBytes) {
  ManualClock clock;
  SimSystem sim(clock, {});
  {
    const auto cfg = one_driver("proxr_relay");
    const auto wire = sim.wire_for_transport("wire", cfg);
    EXPECT_EQ(cut(wire.request, joined({codec::proxr::select_bank(2)->tx, codec::proxr::relay_on(3)->tx})),
              (std::vector<std::size_t>{3, 2}));
  }
  {
    const auto cfg = one_driver("pfeiffer_maxigauge");
    const auto wire = sim.wire_for_transport("wire", cfg);
    EXPECT_EQ(cut(wire.request, joined({codec::maxigauge::read_pressure(1)->tx, codec::maxigauge::enquiry().tx})),
              (std::vector<std::size_t>{5, 1}));
  }
  {
    const auto cfg = one_driver("plc2000_valves");
    const auto wire = sim.wire_for_transport("wire", cfg);
    const auto read = codec::modbus::read_coils(1, 1, 0, 1)->tx;
    const auto write = codec::modbus::write_single_coil(2, 1, 0, true).tx;
    EXPECT_EQ(cut(wire.request, joined({read, write})), (std::vector<std::size_t>{read.size(), write.size()}));
  }
  {
    const auto cfg = one_driver("lakeshore");
    const auto wire = sim.wire_for_transport("wire", cfg);
    const auto read = codec::lakeshore::read_input('A')->tx;
    const auto clear = codec::lakeshore::clear_status().tx;
    EXPECT_EQ(cut(wire.request, joined({clear, read})), (std::vector<std::size_t>{clear.size(), read.size()}));
  }
}

TEST(WireSim, PychronValvesClosesAfterReply) {
  ManualClock clock;
  SimSystem sim(clock, {});
  for (const char* kind : {"pychron_valves", "agilent_switch", "proxr_relay"}) {
    const auto cfg = one_driver(kind);
    EXPECT_EQ(sim.wire_for_transport("wire", cfg).close_after_reply, std::string(kind) == "pychron_valves") << kind;
  }
}

TEST(WireSim, PlcBusJoinsKinds) {
  ManualClock clock;
  SimSystem sim(clock, {});
  config::SystemConfig cfg;
  for (const auto& [name, kind] : {std::pair{"a_valves", "plc2000_valves"}, std::pair{"b_gauges", "plc2000_gauges"},
                                   std::pair{"c_valves", "plc2000_valves"}}) {
    config::DriverConfig d;
    d.name = name;
    d.kind = kind;
    d.transport = "plc";
    cfg.drivers.emplace(d.name, std::move(d));
  }
  const auto wire = sim.wire_for_transport("plc", cfg);
  EXPECT_EQ(wire.emulator, "plc2000_valves+plc2000_gauges");
  const auto read = codec::modbus::read_coils(1, 1, 0, 1)->tx;
  EXPECT_EQ(wire.request(read), read.size());
}

TEST(WireSim, NgxUsesDriversSendTerminator) {
  ManualClock clock;
  SimSystem sim(clock, {});
  toml::table options;
  options.insert("send_terminator", "\n");
  const auto lf = one_driver("ngx_valves", options);
  EXPECT_EQ(sim.wire_for_transport("wire", lf).request(to_bytes("SAB 1\nSAB 0\n")), 6U);
  const auto cr = one_driver("ngx_valves");
  EXPECT_EQ(sim.wire_for_transport("wire", cr).request(to_bytes("SAB 1\n")), std::nullopt);
  EXPECT_EQ(sim.wire_for_transport("wire", cr).request(to_bytes("SAB 1\r")), 6U);
}

TEST(WireSim, HookIsHookForTransports) {
  // The in-process form answers as before: a ProXR board acks a select.
  ManualClock clock;
  SimSystem sim(clock, {});
  const auto cfg = one_driver("proxr_relay");
  const auto hook = sim.hook_for_transport("wire", cfg);
  ASSERT_TRUE(hook);
  EXPECT_EQ(hook(codec::proxr::select_bank(1)->tx), Bytes{codec::proxr::kAck});
}
