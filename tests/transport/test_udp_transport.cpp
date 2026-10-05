// UdpTransport against an in-process UDP peer on the loopback interface.
#include "pychron/transport/udp_transport.hpp"

#include <gtest/gtest.h>

#include <asio.hpp>
#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

using namespace pychron;
using namespace std::chrono_literals;

namespace {

// A UDP socket on 127.0.0.1:<ephemeral> running `session` on its own thread.
// The session answers whoever sent the datagram it received last.
class LoopbackPeer {
 public:
  using Endpoint = asio::ip::udp::endpoint;
  using Session = std::function<void(LoopbackPeer&)>;

  explicit LoopbackPeer(Session session) : socket_(io_, Endpoint(asio::ip::make_address("127.0.0.1"), 0)) {
    port_ = socket_.local_endpoint().port();
    thread_ = std::thread([this, session = std::move(session)] {
      session(*this);
      finished_ = true;
    });
  }
  // A session cut short by a failed expectation may still be waiting in
  // receive(): wake it with datagrams until it ends (closing a socket another
  // thread is blocked on does not wake it everywhere).
  ~LoopbackPeer() {
    asio::io_context io;
    asio::ip::udp::socket waker(io, Endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const Endpoint self(asio::ip::make_address("127.0.0.1"), port_);
    asio::error_code ec;
    while (!finished_) {
      waker.send_to(asio::buffer(std::string("wake")), self, 0, ec);
      std::this_thread::sleep_for(10ms);
    }
    thread_.join();
  }

  std::uint16_t port() const { return port_; }

  // The next datagram's text; empty once the socket is closed.
  std::string receive() {
    std::array<char, 2048> buf{};
    asio::error_code ec;
    const auto n = socket_.receive_from(asio::buffer(buf), client_, 0, ec);
    return ec ? std::string{} : std::string(buf.data(), n);
  }
  void send(const std::string& text) {
    asio::error_code ec;
    socket_.send_to(asio::buffer(text), client_, 0, ec);
  }
  const Endpoint& client() const { return client_; }

 private:
  asio::io_context io_;
  asio::ip::udp::socket socket_;
  Endpoint client_;
  std::uint16_t port_ = 0;
  std::atomic<bool> finished_{false};
  std::thread thread_;
};

TransportOptions opts() {
  TransportOptions o;
  o.name = "udp1";
  o.timeout = 300ms;
  return o;
}

const ReadSpec kCr = ReadSpec::until("\r");

}  // namespace

TEST(UdpTransport, ExchangeWithPeer) {
  LoopbackPeer peer([](LoopbackPeer& p) {
    for (int i = 0; i < 2; ++i) p.send("re:" + p.receive());
  });
  UdpTransport t({"127.0.0.1", peer.port()}, opts());
  ASSERT_TRUE(t.open());
  EXPECT_EQ(to_string(*t.exchange(to_bytes("a\r"), kCr)), "re:a\r");
  EXPECT_EQ(to_string(*t.exchange(to_bytes("b\r"), kCr)), "re:b\r");
  EXPECT_EQ(t.health().state, HealthState::Connected);
}

TEST(UdpTransport, ReplySplitOverDatagramsFramesAndKeepsRemainder) {
  LoopbackPeer peer([](LoopbackPeer& p) {
    p.receive();
    p.send("pa");
    std::this_thread::sleep_for(30ms);
    p.send("rt\rnext\r");
    p.receive();  // hold until the client is done
  });
  UdpTransport t({"127.0.0.1", peer.port()}, opts());
  ASSERT_TRUE(t.open());
  EXPECT_EQ(to_string(*t.exchange(to_bytes("q\r"), kCr)), "part\r");
  EXPECT_EQ(to_string(*t.read(kCr)), "next\r");
  EXPECT_TRUE(t.write(to_bytes("bye\r")));
}

TEST(UdpTransport, LargeDatagramArrivesWhole) {
  // Longer than the 512-byte chunk the stream transports read with.
  const std::string big(4000, 'x');
  LoopbackPeer peer([&](LoopbackPeer& p) { p.send(p.receive() + big + "\r"); });
  UdpTransport t({"127.0.0.1", peer.port()}, opts());
  ASSERT_TRUE(t.open());
  auto r = t.exchange(to_bytes("q"), kCr);
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_EQ(to_string(*r), "q" + big + "\r");
}

TEST(UdpTransport, NoReplyTimesOut) {
  LoopbackPeer peer([](LoopbackPeer& p) {
    p.receive();
    p.receive();
  });
  UdpTransport t({"127.0.0.1", peer.port()}, opts());
  ASSERT_TRUE(t.open());
  const auto start = std::chrono::steady_clock::now();
  auto r = t.exchange(to_bytes("q\r"), kCr, 100ms);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Timeout);
  EXPECT_GE(elapsed, 90ms);
  EXPECT_LT(elapsed, 2s);
  (void)t.write(to_bytes("bye\r"));
}

TEST(UdpTransport, LateReplyIsDiscardedBeforeTheNextExchange) {
  // The first reply arrives after the client gave up on it; the next exchange
  // must not take it for its own.
  LoopbackPeer peer([](LoopbackPeer& p) {
    const auto first = p.receive();
    std::this_thread::sleep_for(150ms);
    p.send("re:" + first);
    p.send("re:" + p.receive());
  });
  UdpTransport t({"127.0.0.1", peer.port()}, opts());
  ASSERT_TRUE(t.open());
  EXPECT_FALSE(t.exchange(to_bytes("one\r"), kCr, 50ms));
  std::this_thread::sleep_for(200ms);  // the late reply is now queued
  EXPECT_EQ(to_string(*t.exchange(to_bytes("two\r"), kCr)), "re:two\r");
}

TEST(UdpTransport, DatagramsFromOtherPeersAreIgnored) {
  LoopbackPeer peer([](LoopbackPeer& p) {
    const auto q = p.receive();
    // A stranger writes to the client's port before the real reply.
    asio::io_context io;
    asio::ip::udp::socket stranger(io, asio::ip::udp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    asio::error_code ec;
    stranger.send_to(asio::buffer(std::string("spoof\r")), p.client(), 0, ec);
    std::this_thread::sleep_for(30ms);
    p.send("re:" + q);
  });
  UdpTransport t({"127.0.0.1", peer.port()}, opts());
  ASSERT_TRUE(t.open());
  EXPECT_EQ(to_string(*t.exchange(to_bytes("q\r"), kCr)), "re:q\r");
}

TEST(UdpTransport, NobodyListeningIsAnErrorNotASilentSuccess) {
  std::uint16_t port = 0;
  {
    asio::io_context io;
    asio::ip::udp::socket s(io, asio::ip::udp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    port = s.local_endpoint().port();
  }
  UdpTransport t({"127.0.0.1", port}, opts());
  ASSERT_TRUE(t.open());  // UDP cannot tell at open
  auto r = t.exchange(to_bytes("q\r"), kCr, 200ms);
  ASSERT_FALSE(r);
  // Linux reports ICMP port unreachable on a connected socket (Io); other
  // systems may just time out.
  EXPECT_TRUE(r.error().kind == ErrorKind::Io || r.error().kind == ErrorKind::Timeout) << r.error().what;
  EXPECT_EQ(r.error().device, "udp1");
}

TEST(UdpTransport, UnresolvableHostIsIoError) {
  UdpTransport t({"invalid host name", 1}, opts());
  auto r = t.open();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Io);
}

TEST(UdpTransport, OversizedMessageIsConfigError) {
  LoopbackPeer peer([](LoopbackPeer& p) { p.receive(); });
  UdpTransport t({"127.0.0.1", peer.port()}, opts());
  ASSERT_TRUE(t.open());
  auto r = t.write(Bytes(UdpTransport::kMaxDatagram + 1, 'x'));
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  (void)t.write(to_bytes("bye\r"));
}
