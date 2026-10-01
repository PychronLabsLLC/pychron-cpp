#include "pychron/transport/factory.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <vector>

#include "pychron/core/config/logging_config.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/transport/serial_transport.hpp"
#include "pychron/transport/tcp_transport.hpp"
#include "pychron/transport/trace.hpp"
#include "pychron/transport/trace_recorder.hpp"

using namespace pychron;
using namespace std::chrono_literals;

namespace {

config::TransportConfig cfg(std::string name, config::TransportKind kind, config::TransportParams params) {
  config::TransportConfig c;
  c.name = std::move(name);
  c.kind = kind;
  c.params = std::move(params);
  c.timeout_ms = 120;
  c.retries = 2;
  return c;
}

}  // namespace

TEST(TransportFactory, SimUsesHookAndConfiguredOptions) {
  TransportContext ctx;
  ctx.sim_hook = [](const Bytes& tx) { return to_bytes("sim:" + to_string(tx)); };
  auto t = make_transport(cfg("gauges", config::TransportKind::Sim, config::SimParams{}), ctx);
  ASSERT_TRUE(t) << to_string(t.error());
  EXPECT_EQ((*t)->name(), "gauges");
  auto* sim = dynamic_cast<SimTransport*>(t->get());
  ASSERT_NE(sim, nullptr);
  EXPECT_EQ(sim->options().timeout, Duration(120ms));
  EXPECT_EQ(sim->options().retries, 2);
  ASSERT_TRUE((*t)->open());
  EXPECT_EQ(to_string(*(*t)->exchange(to_bytes("x\n"), ReadSpec::until("\n"))), "sim:x\n");
}

TEST(TransportFactory, SimWithoutHookIsSilent) {
  auto t = make_transport(cfg("quiet", config::TransportKind::Sim, config::SimParams{}));
  ASSERT_TRUE(t);
  ASSERT_TRUE((*t)->open());
  EXPECT_EQ((*t)->exchange(to_bytes("x"), ReadSpec::fixed(1)).error().kind, ErrorKind::Timeout);
}

TEST(TransportFactory, SerialAndTcpAreBuiltClosed) {
  config::SerialParams sp;
  sp.port = "/dev/null-port";
  sp.baud = 19200;
  sp.parity = config::Parity::Even;
  sp.stop_bits = 2;
  auto s = make_transport(cfg("valve_bus", config::TransportKind::Serial, sp));
  ASSERT_TRUE(s);
  auto* serial = dynamic_cast<SerialTransport*>(s->get());
  ASSERT_NE(serial, nullptr);
  EXPECT_EQ(serial->settings().baud, 19200u);
  EXPECT_EQ(serial->settings().parity, SerialSettings::Parity::Even);
  EXPECT_EQ(serial->settings().stop_bits, 2u);
  EXPECT_EQ((*s)->health().state, HealthState::Down);

  auto t = make_transport(cfg("net", config::TransportKind::Tcp, config::TcpParams{"10.0.0.5", 4001}));
  ASSERT_TRUE(t);
  auto* tcp = dynamic_cast<TcpTransport*>(t->get());
  ASSERT_NE(tcp, nullptr);
  EXPECT_EQ(tcp->settings().host, "10.0.0.5");
  EXPECT_EQ(tcp->settings().port, 4001);
}

TEST(TransportFactory, ModbusIsNotYetSupported) {
  auto t = make_transport(cfg("mb", config::TransportKind::ModbusTcp, config::ModbusTcpParams{}));
  ASSERT_FALSE(t);
  EXPECT_EQ(t.error().kind, ErrorKind::Config);
  EXPECT_EQ(t.error().device, "mb");
}

TEST(TransportFactory, TraceFlagWrapsInRecorder) {
  const auto dir = std::filesystem::temp_directory_path();
  auto c = cfg("traced", config::TransportKind::Sim, config::SimParams{});
  c.trace = true;
  TransportContext ctx;
  ctx.trace_dir = dir.string();
  ctx.sim_hook = [](const Bytes&) { return to_bytes("ok\n"); };
  {
    auto t = make_transport(c, ctx);
    ASSERT_TRUE(t);
    ASSERT_NE(dynamic_cast<TraceRecorder*>(t->get()), nullptr);
    ASSERT_TRUE((*t)->open());
    ASSERT_TRUE((*t)->exchange(to_bytes("q\n"), ReadSpec::until("\n")));
  }
  const auto path = dir / "traced.trace";
  auto records = load_trace(path.string());
  ASSERT_TRUE(records);
  EXPECT_EQ(records->size(), 2u);
  std::filesystem::remove(path);
}

TEST(TransportFactory, TracedTransportMirrorsToWireLogger) {
  ManualClock clock;
  SignalBus bus;
  std::vector<Log> got;
  auto sub = bus.subscribe<Log>([&](const Log& e) { got.push_back(e); });
  config::LoggingConfig lc;
  lc.default_level = LogLevel::Trace;
  auto hub = LogHub::create(lc, clock, &bus);
  ASSERT_TRUE(hub);

  const auto dir = std::filesystem::temp_directory_path() / "pychron_factory_wire_test";
  std::filesystem::create_directories(dir);
  TransportContext ctx;
  ctx.clock = &clock;
  ctx.log_hub = *hub;
  ctx.trace_dir = dir.string();
  ctx.sim_hook = [](const Bytes&) { return to_bytes("ok\n"); };
  auto c = cfg("wired", config::TransportKind::Sim, config::SimParams{});
  c.trace = true;
  auto t = make_transport(c, ctx);
  ASSERT_TRUE(t) << to_string(t.error());
  ASSERT_TRUE((*t)->open());
  ASSERT_TRUE((*t)->exchange(to_bytes("q\n"), ReadSpec::until("\n")));
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0].logger, "wired.wire");
  EXPECT_EQ(got[0].message, "tx 2B 71 0A |q.|");
  t->reset();
  std::filesystem::remove_all(dir);
}
