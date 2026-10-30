// TcpListener and UdpListener against the transports, on the loopback
// interface in real time. Bounds are upper bounds of 5 s or more, there only
// to tell an answer from a stall.
#include "pychron/transport/listener.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "pychron/transport/tcp_transport.hpp"
#include "pychron/transport/udp_transport.hpp"

using namespace pychron;
using namespace std::chrono_literals;

namespace {

TransportOptions client_options() {
  TransportOptions o;
  o.name = "client";
  o.timeout = 5s;
  return o;
}

}  // namespace

TEST(ListenerSteady, TcpBindsPortZeroAndReportsPort) {
  auto listener = TcpListener::bind("127.0.0.1", 0);
  ASSERT_TRUE(listener) << listener.error().what;
  EXPECT_NE((*listener)->port(), 0);
  TcpTransport client({"127.0.0.1", (*listener)->port()}, client_options());
  EXPECT_TRUE(client.open());
}

TEST(ListenerSteady, TcpAcceptedStreamEchoes) {
  auto listener = TcpListener::bind("127.0.0.1", 0);
  ASSERT_TRUE(listener);
  std::thread server([&] {
    auto stream = (*listener)->accept(5s);
    ASSERT_TRUE(stream) << stream.error().what;
    Bytes got;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (got.size() < 4 && std::chrono::steady_clock::now() < deadline) {
      auto some = (*stream)->read_some(100ms);
      ASSERT_TRUE(some) << some.error().what;
      got.insert(got.end(), some->begin(), some->end());
    }
    EXPECT_EQ(to_string(got), "ping");
    EXPECT_TRUE((*stream)->write(to_bytes("pong\n"), 5s));
  });
  TcpTransport client({"127.0.0.1", (*listener)->port()}, client_options());
  ASSERT_TRUE(client.open());
  auto reply = client.exchange(to_bytes("ping"), ReadSpec::until("\n"));
  server.join();
  ASSERT_TRUE(reply) << reply.error().what;
  EXPECT_EQ(to_string(*reply), "pong\n");
}

TEST(ListenerSteady, TcpReadSomeTimesOutEmpty) {
  auto listener = TcpListener::bind("127.0.0.1", 0);
  ASSERT_TRUE(listener);
  TcpTransport client({"127.0.0.1", (*listener)->port()}, client_options());
  ASSERT_TRUE(client.open());
  auto stream = (*listener)->accept(5s);
  ASSERT_TRUE(stream);
  const auto start = std::chrono::steady_clock::now();
  auto some = (*stream)->read_some(50ms);
  ASSERT_TRUE(some) << some.error().what;
  EXPECT_TRUE(some->empty());
  EXPECT_LT(std::chrono::steady_clock::now() - start, 5s);
}

TEST(ListenerSteady, TcpPeerCloseIsIo) {
  auto listener = TcpListener::bind("127.0.0.1", 0);
  ASSERT_TRUE(listener);
  auto client = std::make_unique<TcpTransport>(TcpSettings{"127.0.0.1", (*listener)->port()}, client_options());
  ASSERT_TRUE(client->open());
  auto stream = (*listener)->accept(5s);
  ASSERT_TRUE(stream);
  client.reset();
  Result<Bytes> some = Bytes{};
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (some && std::chrono::steady_clock::now() < deadline) some = (*stream)->read_some(100ms);
  ASSERT_FALSE(some);
  EXPECT_EQ(some.error().kind, ErrorKind::Io);
}

TEST(ListenerSteady, TcpCloseWhileAcceptWaits) {
  auto listener = TcpListener::bind("127.0.0.1", 0);
  ASSERT_TRUE(listener);
  Result<std::unique_ptr<ByteStream>> accepted = fail(ErrorKind::Io, "not run");
  const auto start = std::chrono::steady_clock::now();
  std::thread waiter([&] { accepted = (*listener)->accept(10s); });
  std::this_thread::sleep_for(100ms);
  (*listener)->close();
  waiter.join();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 5s);
  ASSERT_FALSE(accepted);
  EXPECT_EQ(accepted.error().kind, ErrorKind::Cancelled);
}

TEST(ListenerSteady, TcpCloseWhileReadWaits) {
  auto listener = TcpListener::bind("127.0.0.1", 0);
  ASSERT_TRUE(listener);
  TcpTransport client({"127.0.0.1", (*listener)->port()}, client_options());
  ASSERT_TRUE(client.open());
  auto stream = (*listener)->accept(5s);
  ASSERT_TRUE(stream);
  Result<Bytes> read = Bytes{};
  const auto start = std::chrono::steady_clock::now();
  std::thread reader([&] { read = (*stream)->read_some(10s); });
  std::this_thread::sleep_for(100ms);
  (*stream)->close();
  reader.join();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 5s);
  ASSERT_FALSE(read);
  EXPECT_EQ(read.error().kind, ErrorKind::Cancelled);
}

TEST(ListenerSteady, TcpBindTakenPortFails) {
  auto first = TcpListener::bind("127.0.0.1", 0);
  ASSERT_TRUE(first);
  const auto port = (*first)->port();
  auto second = TcpListener::bind("127.0.0.1", port);
  ASSERT_FALSE(second);
  EXPECT_EQ(second.error().kind, ErrorKind::Io);
  EXPECT_NE(second.error().what.find(std::to_string(port)), std::string::npos) << second.error().what;
}

TEST(ListenerSteady, BadAddressFails) {
  auto listener = TcpListener::bind("not an address", 0);
  ASSERT_FALSE(listener);
  EXPECT_EQ(listener.error().kind, ErrorKind::Io);
}

TEST(ListenerSteady, UdpReceivesAndRepliesToSender) {
  auto listener = UdpListener::bind("127.0.0.1", 0);
  ASSERT_TRUE(listener) << listener.error().what;
  EXPECT_NE((*listener)->port(), 0);
  std::thread server([&] {
    auto d = (*listener)->receive(5s);
    ASSERT_TRUE(d) << d.error().what;
    EXPECT_EQ(to_string(d->data), "ping");
    EXPECT_TRUE((*listener)->send_to(*d->from, to_bytes("pong\n")));
  });
  UdpTransport client({"127.0.0.1", (*listener)->port()}, client_options());
  ASSERT_TRUE(client.open());
  auto reply = client.exchange(to_bytes("ping"), ReadSpec::until("\n"));
  server.join();
  ASSERT_TRUE(reply) << reply.error().what;
  EXPECT_EQ(to_string(*reply), "pong\n");
}

TEST(ListenerSteady, UdpReceiveTimesOut) {
  auto listener = UdpListener::bind("127.0.0.1", 0);
  ASSERT_TRUE(listener);
  auto d = (*listener)->receive(50ms);
  ASSERT_FALSE(d);
  EXPECT_EQ(d.error().kind, ErrorKind::Timeout);
}
