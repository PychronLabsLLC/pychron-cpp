#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "pychron/transport/transport.hpp"

namespace pychron {

struct UdpSettings {
  std::string host;
  std::uint16_t port = 0;
};

// UDP client socket via asio (Qtegra RemoteControl at labs that run it over
// UDP). open() resolves the peer and connects the socket to it, so the kernel
// drops datagrams from any other address. One write is one datagram; a read
// gathers datagrams until the ReadSpec is satisfied, so a reply split over
// several datagrams still frames. A peer that is not listening shows up as an
// Io error on the next read (ICMP port unreachable), not at open().
class UdpTransport final : public QueuedTransport {
 public:
  // The largest datagram UDP over IPv4 carries.
  static constexpr std::size_t kMaxDatagram = 65507;

  UdpTransport(UdpSettings settings, TransportOptions options);
  ~UdpTransport() override;

  const UdpSettings& settings() const noexcept { return settings_; }

 private:
  Result<void> do_open() override;
  void do_close() override;
  void do_discard_input() override;
  Result<void> do_write(const Bytes& tx, Duration timeout) override;
  Result<Bytes> do_read(const ReadSpec& rs, Duration timeout) override;

  struct Io;
  UdpSettings settings_;
  std::unique_ptr<Io> io_;
};

}  // namespace pychron
