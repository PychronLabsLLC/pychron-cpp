#include "pychron/transport/serial_transport.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <cstdlib>
#include <unistd.h>
#endif

using namespace pychron;
using namespace std::chrono_literals;

namespace {

TransportOptions opts() {
  TransportOptions o;
  o.name = "ser1";
  o.timeout = 300ms;
  return o;
}

}  // namespace

TEST(SerialTransport, MissingPortIsIoError) {
#ifdef _WIN32
  SerialSettings s{"COM250"};
#else
  SerialSettings s{"/dev/pychron-no-such-port"};
#endif
  SerialTransport t(s, opts());
  auto r = t.open();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Io);
  EXPECT_EQ(r.error().device, "ser1");
  EXPECT_EQ(t.exchange(to_bytes("x"), ReadSpec::fixed(1)).error().kind, ErrorKind::NotConnected);
}

#ifndef _WIN32

namespace {

// Pseudo-terminal pair: the transport opens the slave, the test drives the master.
class Pty {
 public:
  Pty() {
    master_ = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (master_ >= 0 && ::grantpt(master_) == 0 && ::unlockpt(master_) == 0) {
      if (const char* name = ::ptsname(master_)) slave_name_ = name;
    }
  }
  ~Pty() {
    if (master_ >= 0) ::close(master_);
  }
  bool ok() const { return !slave_name_.empty(); }
  const std::string& slave_name() const { return slave_name_; }

  std::string read_until(char term, std::chrono::milliseconds timeout = 1000ms) {
    std::string out;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      pollfd p{master_, POLLIN, 0};
      if (::poll(&p, 1, 20) <= 0) continue;
      char c = 0;
      if (::read(master_, &c, 1) != 1) break;
      out += c;
      if (c == term) break;
    }
    return out;
  }
  void write(const std::string& s) { (void)!::write(master_, s.data(), s.size()); }

 private:
  int master_ = -1;
  std::string slave_name_;
};

}  // namespace

TEST(SerialTransport, ExchangeOverPty) {
  Pty pty;
  ASSERT_TRUE(pty.ok());
  SerialTransport t({pty.slave_name(), 115200}, opts());
  auto opened = t.open();
  ASSERT_TRUE(opened) << to_string(opened.error());

  std::thread device([&] {
    const auto req = pty.read_until('\r');
    pty.write(req == "PR1\r" ? "0,1.5E-08\r" : "ERR\r");
  });
  auto r = t.exchange(to_bytes("PR1\r"), ReadSpec::until("\r"));
  device.join();
  ASSERT_TRUE(r) << to_string(r.error());
  EXPECT_EQ(to_string(*r), "0,1.5E-08\r");
  EXPECT_EQ(t.health().state, HealthState::Connected);
}

TEST(SerialTransport, NoReplyTimesOut) {
  Pty pty;
  ASSERT_TRUE(pty.ok());
  SerialTransport t({pty.slave_name(), 9600}, opts());
  ASSERT_TRUE(t.open());
  auto r = t.exchange(to_bytes("Q\r"), ReadSpec::fixed(4), 80ms);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Timeout);
  t.close();
  EXPECT_EQ(t.health().state, HealthState::Down);
}

#endif
