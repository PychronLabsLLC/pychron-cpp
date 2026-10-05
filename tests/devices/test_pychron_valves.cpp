// pychron_valves against a simulated legacy valve service, on the simulated
// wire and on a real loopback socket that hangs up after every reply
// (plan 2026-10-05, task A4).

#include "pychron/devices/pychron_valves.hpp"

#include <gtest/gtest.h>

#include <asio.hpp>
#include <atomic>
#include <chrono>
#include <thread>

#include "pychron/devices/driver_registry.hpp"
#include "pychron/devices/pychron_valve_server_sim.hpp"
#include "pychron/transport/tcp_transport.hpp"
#include "valve_conformance.hpp"

using namespace pychron;
using namespace std::chrono_literals;

namespace {

TransportOptions fast(std::string name = "felix") {
  TransportOptions o;
  o.name = std::move(name);
  o.timeout = 100ms;
  return o;
}

struct Rig final : pychron::test::ValveRig {
  Rig() { EXPECT_TRUE(bus->open()); }
  PychronValveServerSim server;
  pychron::test::WireTap wire{server.hook(), to_bytes("No Response")};
  std::unique_ptr<SimTransport> bus = SimTransport::hooked(wire.hook(), fast());
  PychronValves driver{"felix_valves", *bus};

  IValveActuator& actuator() override { return driver; }
  ValveAddress first() override { return {"A"}; }
  ValveAddress second() override { return {"Inlet"}; }
  std::optional<ValveAddress> bad() override { return ValveAddress{"A,B"}; }
  pychron::test::WireTap* tap() override { return &wire; }
};

// Legacy's server over TCP: accept, read one command, answer, hang up.
class OneShotServer {
 public:
  explicit OneShotServer(PychronValveServerSim& sim)
      : acceptor_(io_, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)) {
    port_ = acceptor_.local_endpoint().port();
    thread_ = std::thread([this, &sim] {
      for (;;) {
        asio::error_code ec;
        asio::ip::tcp::socket socket(io_);
        acceptor_.accept(socket, ec);
        if (ec || stop_) return;
        ++connections;
        std::string line;
        char c = 0;
        while (asio::read(socket, asio::buffer(&c, 1), ec) == 1 && !ec && c != '\r') line += c;
        const Bytes reply = sim.respond(to_bytes(line + "\r"));
        asio::write(socket, asio::buffer(reply), ec);
        socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        socket.close(ec);
      }
    });
  }
  // A blocked accept() is woken by a connection, not by closing the acceptor.
  ~OneShotServer() {
    stop_ = true;
    asio::error_code ec;
    asio::io_context io;
    asio::ip::tcp::socket poke(io);
    poke.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port_), ec);
    thread_.join();
    acceptor_.close(ec);
  }
  std::uint16_t port() const { return port_; }
  std::atomic<int> connections{0};

 private:
  asio::io_context io_;
  asio::ip::tcp::acceptor acceptor_;
  std::uint16_t port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

}  // namespace

TEST(PychronValves, PassTheValveConformanceSuite) {
  Rig rig;
  pychron::test::expect_valve_conformance(rig);
}

TEST(PychronValves, OneConnectionPerCommandOnARealSocket) {
  PychronValveServerSim sim;
  OneShotServer server(sim);
  TcpTransport tcp({"127.0.0.1", server.port()}, fast());
  PychronValves valves("felix_valves", tcp);
  ASSERT_TRUE(valves.open(ValveAddress{"A"}));
  EXPECT_TRUE(sim.is_open("A"));
  EXPECT_EQ(*valves.read(ValveAddress{"A"}), ValveState::Open);
  ASSERT_TRUE(valves.close(ValveAddress{"A"}));
  EXPECT_EQ(*valves.read(ValveAddress{"A"}), ValveState::Closed);
  EXPECT_EQ(server.connections, 4);
}

TEST(PychronValves, TheRemotesRefusalsKeepTheirMeaning) {
  Rig rig;
  rig.server.declare({"A", "B"});
  rig.server.lock("B");
  auto locked = rig.driver.open(ValveAddress{"B"});
  ASSERT_FALSE(locked);
  EXPECT_EQ(locked.error().kind, ErrorKind::Interlock);
  EXPECT_EQ(locked.error().device, "felix_valves");
  auto unknown = rig.driver.read(ValveAddress{"Z"});
  ASSERT_FALSE(unknown);
  EXPECT_EQ(unknown.error().kind, ErrorKind::Config);
  EXPECT_NE(unknown.error().what.find("Z is not a registered valve name"), std::string::npos) << unknown.error().what;
}

TEST(PychronValves, AnAlreadyOpenValveIsStillSuccess) {
  Rig rig;
  ASSERT_TRUE(rig.driver.open(ValveAddress{"A"}));
  ASSERT_TRUE(rig.driver.open(ValveAddress{"A"}));  // the server answers "ok"
}

TEST(PychronValves, TheRegistryBuildsIt) {
  PychronValveServerSim sim;
  auto bus = SimTransport::hooked(sim.hook(), fast());
  ASSERT_TRUE(bus->open());
  auto dev = DriverRegistry::global().create("pychron_valves", *bus, toml::table{}, DriverContext{"felix_valves"});
  ASSERT_TRUE(dev) << dev.error().what;
  ASSERT_NE(capability<IValveActuator>(**dev), nullptr);
}
