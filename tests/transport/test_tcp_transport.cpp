// TcpTransport against an in-process server on the loopback interface.
#include "pychron/transport/tcp_transport.hpp"

#include <gtest/gtest.h>

#include <asio.hpp>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

using namespace pychron;
using namespace std::chrono_literals;

namespace {

// Accepts one connection on 127.0.0.1:<ephemeral> and runs `session` on it.
class LoopbackServer {
 public:
  using Session = std::function<void(asio::ip::tcp::socket&)>;

  explicit LoopbackServer(Session session)
      : acceptor_(io_, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)) {
    port_ = acceptor_.local_endpoint().port();
    thread_ = std::thread([this, session = std::move(session)] {
      asio::error_code ec;
      asio::ip::tcp::socket socket(io_);
      acceptor_.accept(socket, ec);
      if (!ec) session(socket);
      socket.close(ec);
    });
  }
  ~LoopbackServer() {
    asio::error_code ec;
    acceptor_.close(ec);
    thread_.join();
  }

  std::uint16_t port() const { return port_; }

 private:
  asio::io_context io_;
  asio::ip::tcp::acceptor acceptor_;
  std::uint16_t port_ = 0;
  std::thread thread_;
};

std::string read_line(asio::ip::tcp::socket& s) {
  std::string line;
  char c = 0;
  asio::error_code ec;
  while (asio::read(s, asio::buffer(&c, 1), ec) == 1 && !ec) {
    line += c;
    if (c == '\n') break;
  }
  return line;
}

void send(asio::ip::tcp::socket& s, const std::string& text) {
  asio::error_code ec;
  asio::write(s, asio::buffer(text), ec);
}

TransportOptions opts() {
  TransportOptions o;
  o.name = "tcp1";
  o.timeout = 300ms;
  return o;
}

const ReadSpec kLf = ReadSpec::until("\n");

}  // namespace

TEST(TcpTransport, ExchangeWithServer) {
  LoopbackServer server([](asio::ip::tcp::socket& s) {
    for (int i = 0; i < 2; ++i) send(s, "re:" + read_line(s));
  });
  TcpTransport t({"127.0.0.1", server.port()}, opts());
  ASSERT_TRUE(t.open());
  EXPECT_EQ(to_string(*t.exchange(to_bytes("a\n"), kLf)), "re:a\n");
  EXPECT_EQ(to_string(*t.exchange(to_bytes("b\n"), kLf)), "re:b\n");
  EXPECT_EQ(t.health().state, HealthState::Connected);
}

TEST(TcpTransport, ReassemblesFrameAcrossSegmentsAndKeepsRemainder) {
  LoopbackServer server([](asio::ip::tcp::socket& s) {
    read_line(s);
    send(s, "pa");
    std::this_thread::sleep_for(30ms);
    send(s, "rt\nnext\n");
    read_line(s);  // hold the connection until the client is done
  });
  TcpTransport t({"127.0.0.1", server.port()}, opts());
  ASSERT_TRUE(t.open());
  EXPECT_EQ(to_string(*t.exchange(to_bytes("q\n"), kLf)), "part\n");
  EXPECT_EQ(to_string(*t.read(kLf)), "next\n");
  EXPECT_TRUE(t.write(to_bytes("bye\n")));
}

TEST(TcpTransport, NoReplyTimesOut) {
  LoopbackServer server([](asio::ip::tcp::socket& s) {
    read_line(s);
    read_line(s);
  });
  TcpTransport t({"127.0.0.1", server.port()}, opts());
  ASSERT_TRUE(t.open());
  const auto start = std::chrono::steady_clock::now();
  auto r = t.exchange(to_bytes("q\n"), kLf, 100ms);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Timeout);
  EXPECT_GE(elapsed, 90ms);
  EXPECT_LT(elapsed, 2s);
  (void)t.write(to_bytes("bye\n"));
}

TEST(TcpTransport, StaleInputIsDiscardedBeforeExchange) {
  LoopbackServer server([](asio::ip::tcp::socket& s) {
    send(s, "stale\n");
    send(s, "re:" + read_line(s));
  });
  TcpTransport t({"127.0.0.1", server.port()}, opts());
  ASSERT_TRUE(t.open());
  std::this_thread::sleep_for(50ms);  // stale line arrives
  EXPECT_EQ(to_string(*t.exchange(to_bytes("q\n"), kLf)), "re:q\n");
}

TEST(TcpTransport, PeerCloseIsIoError) {
  LoopbackServer server([](asio::ip::tcp::socket& s) { read_line(s); });
  TcpTransport t({"127.0.0.1", server.port()}, opts());
  ASSERT_TRUE(t.open());
  auto r = t.exchange(to_bytes("q\n"), kLf);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Io);
  EXPECT_EQ(r.error().device, "tcp1");
}

TEST(TcpTransport, ConnectionRefusedIsIoError) {
  std::uint16_t port = 0;
  {
    asio::io_context io;
    asio::ip::tcp::acceptor a(io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    port = a.local_endpoint().port();
  }
  TcpTransport t({"127.0.0.1", port}, opts());
  auto r = t.open();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Io);
  EXPECT_EQ(t.health().state, HealthState::Down);
}

TEST(TcpTransport, UnresolvableHostIsIoError) {
  TcpTransport t({"invalid host name", 1}, opts());
  auto r = t.open();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Io);
}
