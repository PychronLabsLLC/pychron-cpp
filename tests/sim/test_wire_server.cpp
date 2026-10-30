// WireServer on the loopback interface, in real time (sim serve spec 7). The
// clients are the real transports. Bounds are upper bounds of 5 s or more,
// there only to tell an answer from a stall.
#include "pychron/sim/wire_server.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "pychron/core/virtual_clock.hpp"
#include "pychron/transport/tcp_transport.hpp"
#include "pychron/transport/udp_transport.hpp"

namespace {

using namespace pychron;
using namespace std::chrono_literals;
using sim::WireServer;
using sim::WireSim;

// Answers each line with "<line>!", counting calls in `calls`.
WireSim echo(const std::shared_ptr<std::atomic<int>>& calls) {
  WireSim w;
  w.request = sim::framer(ReadSpec::until("\n"));
  w.hook = [calls](const Bytes& tx) {
    ++*calls;
    Bytes reply = tx;
    reply.push_back('!');
    return reply;
  };
  w.emulator = "echo";
  return w;
}

TransportOptions client_options(Duration timeout = 5s) {
  TransportOptions o;
  o.name = "client";
  o.timeout = timeout;
  return o;
}

std::unique_ptr<TcpTransport> connect(std::uint16_t port, Duration timeout = 5s) {
  auto t = std::make_unique<TcpTransport>(TcpSettings{"127.0.0.1", port}, client_options(timeout));
  EXPECT_TRUE(t->open());
  return t;
}

// Two consecutive ports nothing is bound to, as far as one can tell.
std::optional<std::uint16_t> free_pair() {
  for (int attempt = 0; attempt < 20; ++attempt) {
    auto a = TcpListener::bind("127.0.0.1", 0);
    if (!a || (*a)->port() >= 65535) continue;
    const auto base = (*a)->port();
    a->reset();
    auto first = TcpListener::bind("127.0.0.1", base);
    auto second = TcpListener::bind("127.0.0.1", static_cast<std::uint16_t>(base + 1));
    if (first && second) return base;
  }
  return std::nullopt;
}

}  // namespace

TEST(WireServerSteady, SplitRequestAnswersOnce) {
  auto calls = std::make_shared<std::atomic<int>>(0);
  WireServer server({});
  ASSERT_TRUE(server.add("dev", WireProtocol::Tcp, echo(calls)));
  auto endpoints = server.start();
  ASSERT_TRUE(endpoints) << endpoints.error().what;
  EXPECT_EQ(endpoints->at("dev").emulator, "echo");
  EXPECT_EQ(endpoints->at("dev").host, "127.0.0.1");
  auto client = connect(endpoints->at("dev").port);
  ASSERT_TRUE(client->write(to_bytes("ab")));
  std::this_thread::sleep_for(100ms);  // the halves reach the server apart
  auto reply = client->exchange(to_bytes("c\n"), ReadSpec::until("!"));
  ASSERT_TRUE(reply) << reply.error().what;
  EXPECT_EQ(to_string(*reply), "abc\n!");
  EXPECT_EQ(*calls, 1);
}

TEST(WireServerSteady, TwoRequestsOneSegmentAnswerInOrder) {
  auto calls = std::make_shared<std::atomic<int>>(0);
  WireServer server({});
  ASSERT_TRUE(server.add("dev", WireProtocol::Tcp, echo(calls)));
  auto endpoints = server.start();
  ASSERT_TRUE(endpoints);
  auto client = connect(endpoints->at("dev").port);
  auto first = client->exchange(to_bytes("a\nb\n"), ReadSpec::until("!"));
  ASSERT_TRUE(first) << first.error().what;
  EXPECT_EQ(to_string(*first), "a\n!");
  auto second = client->read(ReadSpec::until("!"));
  ASSERT_TRUE(second) << second.error().what;
  EXPECT_EQ(to_string(*second), "b\n!");
}

TEST(WireServerSteady, UdpDatagramIsOneRequest) {
  auto calls = std::make_shared<std::atomic<int>>(0);
  WireServer server({});
  ASSERT_TRUE(server.add("dev", WireProtocol::Udp, echo(calls)));
  auto endpoints = server.start();
  ASSERT_TRUE(endpoints) << endpoints.error().what;
  EXPECT_EQ(endpoints->at("dev").protocol, WireProtocol::Udp);
  UdpTransport client({"127.0.0.1", endpoints->at("dev").port}, client_options());
  ASSERT_TRUE(client.open());
  auto reply = client.exchange(to_bytes("ping"), ReadSpec::until("!"));
  ASSERT_TRUE(reply) << reply.error().what;
  EXPECT_EQ(to_string(*reply), "ping!");
}

TEST(WireServerSteady, UnsolicitedArrivesWithoutRequest) {
  auto calls = std::make_shared<std::atomic<int>>(0);
  auto sent = std::make_shared<std::atomic<bool>>(false);
  WireSim w = echo(calls);
  w.unsolicited = [sent] { return sent->exchange(true) ? Bytes{} : to_bytes("evt\n"); };
  WireServer server({});
  ASSERT_TRUE(server.add("dev", WireProtocol::Tcp, std::move(w)));
  auto endpoints = server.start();
  ASSERT_TRUE(endpoints);
  auto client = connect(endpoints->at("dev").port);
  auto event = client->read(ReadSpec::until("\n"));
  ASSERT_TRUE(event) << event.error().what;
  EXPECT_EQ(to_string(*event), "evt\n");
  EXPECT_EQ(*calls, 0);
}

TEST(WireServerSteady, OnConnectRunsPerConnection) {
  auto calls = std::make_shared<std::atomic<int>>(0);
  auto connects = std::make_shared<std::atomic<int>>(0);
  WireSim w = echo(calls);
  w.on_connect = [connects] { ++*connects; };
  WireServer server({});
  ASSERT_TRUE(server.add("dev", WireProtocol::Tcp, std::move(w)));
  auto endpoints = server.start();
  ASSERT_TRUE(endpoints);
  for (int i = 0; i < 2; ++i) {
    auto client = connect(endpoints->at("dev").port);
    ASSERT_TRUE(client->exchange(to_bytes("x\n"), ReadSpec::until("!")));
  }
  EXPECT_EQ(*connects, 2);
}

TEST(WireServerSteady, TwoConnectionsShareModel) {
  // The emulator counts; each connection sees the count the other left.
  auto count = std::make_shared<int>(0);
  WireSim w;
  w.request = sim::framer(ReadSpec::until("\n"));
  w.hook = [count](const Bytes&) { return to_bytes(std::to_string(++*count) + "\n"); };
  WireServer server({});
  ASSERT_TRUE(server.add("dev", WireProtocol::Tcp, std::move(w)));
  auto endpoints = server.start();
  ASSERT_TRUE(endpoints);
  auto a = connect(endpoints->at("dev").port);
  auto b = connect(endpoints->at("dev").port);
  EXPECT_EQ(to_string(*a->exchange(to_bytes("x\n"), ReadSpec::until("\n"))), "1\n");
  EXPECT_EQ(to_string(*b->exchange(to_bytes("x\n"), ReadSpec::until("\n"))), "2\n");
  EXPECT_EQ(to_string(*a->exchange(to_bytes("x\n"), ReadSpec::until("\n"))), "3\n");
}

TEST(WireServerSteady, StartAllOrNothingWhenPortTaken) {
  auto taken = TcpListener::bind("127.0.0.1", 0);
  ASSERT_TRUE(taken);
  const auto port = (*taken)->port();
  if (port == 65535) GTEST_SKIP() << "the OS chose the last port";
  auto calls = std::make_shared<std::atomic<int>>(0);
  WireServer server({.bind = "127.0.0.1", .port_base = port, .clock = nullptr});
  ASSERT_TRUE(server.add("b", WireProtocol::Tcp, echo(calls)));
  ASSERT_TRUE(server.add("a", WireProtocol::Tcp, echo(calls)));
  auto endpoints = server.start();
  ASSERT_FALSE(endpoints);
  EXPECT_EQ(endpoints.error().kind, ErrorKind::Io);
  EXPECT_NE(endpoints.error().what.find("'a'"), std::string::npos) << endpoints.error().what;
  EXPECT_NE(endpoints.error().what.find(std::to_string(port)), std::string::npos) << endpoints.error().what;
  // Nothing is left listening for `b`.
  TcpTransport client({"127.0.0.1", static_cast<std::uint16_t>(port + 1)}, client_options(1s));
  EXPECT_FALSE(client.open());
}

TEST(WireServerSteady, PortBaseAssignsInNameOrder) {
  const auto base = free_pair();
  if (!base) GTEST_SKIP() << "no two free consecutive ports";
  auto calls = std::make_shared<std::atomic<int>>(0);
  WireServer server({.bind = "127.0.0.1", .port_base = *base, .clock = nullptr});
  ASSERT_TRUE(server.add("b", WireProtocol::Tcp, echo(calls)));
  ASSERT_TRUE(server.add("a", WireProtocol::Udp, echo(calls)));
  auto endpoints = server.start();
  ASSERT_TRUE(endpoints) << endpoints.error().what;
  EXPECT_EQ(endpoints->at("a").port, *base);
  EXPECT_EQ(endpoints->at("b").port, *base + 1);
}

TEST(WireServerSteady, RefusesVirtualClock) {
  VirtualClock clock;
  auto calls = std::make_shared<std::atomic<int>>(0);
  WireServer server({.bind = "127.0.0.1", .port_base = 0, .clock = &clock});
  ASSERT_TRUE(server.add("dev", WireProtocol::Tcp, echo(calls)));
  auto endpoints = server.start();
  ASSERT_FALSE(endpoints);
  EXPECT_EQ(endpoints.error().kind, ErrorKind::Config);
  EXPECT_NE(endpoints.error().what.find("real time only"), std::string::npos) << endpoints.error().what;
}

TEST(WireServerSteady, AddRefusesTwiceAndAfterStart) {
  auto calls = std::make_shared<std::atomic<int>>(0);
  WireServer server({});
  ASSERT_TRUE(server.add("dev", WireProtocol::Tcp, echo(calls)));
  EXPECT_FALSE(server.add("dev", WireProtocol::Tcp, echo(calls)));
  ASSERT_TRUE(server.start());
  EXPECT_FALSE(server.add("other", WireProtocol::Tcp, echo(calls)));
}

TEST(WireServerSteady, HalfRequestThenDisconnect) {
  auto calls = std::make_shared<std::atomic<int>>(0);
  WireServer server({});
  ASSERT_TRUE(server.add("dev", WireProtocol::Tcp, echo(calls)));
  auto endpoints = server.start();
  ASSERT_TRUE(endpoints);
  {
    auto quitter = connect(endpoints->at("dev").port);
    ASSERT_TRUE(quitter->write(to_bytes("ab")));
  }
  auto client = connect(endpoints->at("dev").port);
  auto reply = client->exchange(to_bytes("c\n"), ReadSpec::until("!"));
  ASSERT_TRUE(reply) << reply.error().what;
  EXPECT_EQ(to_string(*reply), "c\n!");
}

TEST(WireServerSteady, StopClosesConnections) {
  WireSim silent;
  silent.request = sim::framer(ReadSpec::until("\n"));
  silent.hook = [](const Bytes&) { return Bytes{}; };
  WireServer server({});
  ASSERT_TRUE(server.add("dev", WireProtocol::Tcp, std::move(silent)));
  auto endpoints = server.start();
  ASSERT_TRUE(endpoints);
  auto client = connect(endpoints->at("dev").port, 10s);
  Result<Bytes> reply = Bytes{};
  const auto start = std::chrono::steady_clock::now();
  std::thread asker([&] { reply = client->exchange(to_bytes("x\n"), ReadSpec::until("\n")); });
  std::this_thread::sleep_for(200ms);
  server.stop();
  asker.join();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 5s);
  ASSERT_FALSE(reply);
  EXPECT_EQ(reply.error().kind, ErrorKind::Io) << reply.error().what;
}

TEST(WireServerSteady, CloseAfterReply) {
  auto calls = std::make_shared<std::atomic<int>>(0);
  WireSim w = echo(calls);
  w.close_after_reply = true;
  WireServer server({});
  ASSERT_TRUE(server.add("dev", WireProtocol::Tcp, std::move(w)));
  auto endpoints = server.start();
  ASSERT_TRUE(endpoints);
  TcpTransport client({"127.0.0.1", endpoints->at("dev").port}, client_options());
  for (int i = 0; i < 20; ++i) {
    ASSERT_TRUE(client.open()) << i;
    auto reply = client.exchange(to_bytes("q\n"), ReadSpec::until_close());
    ASSERT_TRUE(reply) << i << ": " << reply.error().what;
    EXPECT_EQ(to_string(*reply), "q\n!");
    client.close();
  }
  EXPECT_EQ(*calls, 20);
}
