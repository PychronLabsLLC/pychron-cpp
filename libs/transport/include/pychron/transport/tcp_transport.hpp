#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "pychron/transport/transport.hpp"

namespace pychron {

struct TcpSettings {
  std::string host;
  std::uint16_t port = 0;
};

// What keepalive the open socket has, as read back from it.
struct TcpKeepalive {
  bool on = false;
  // Seconds of silence before the first probe, where the platform lets it be
  // read back (Linux, macOS); empty elsewhere.
  std::optional<int> idle_seconds;
};

// TCP client socket via asio. open() resolves and connects within the
// configured timeout, then turns keepalive on (idle 10 s, interval 5 s,
// count 3 where the platform allows): a peer that vanished between commands
// is found by the kernel, on real time, and the next call fails Io.
class TcpTransport final : public QueuedTransport {
 public:
  TcpTransport(TcpSettings settings, TransportOptions options);
  ~TcpTransport() override;

  const TcpSettings& settings() const noexcept { return settings_; }

  // The socket's keepalive settings, read on the worker; NotConnected when
  // the transport is not open. For tests and diagnostics.
  [[nodiscard]] Result<TcpKeepalive> keepalive();

 private:
  Result<void> do_open() override;
  void do_close() override;
  void do_discard_input() override;
  Result<void> do_write(const Bytes& tx, Duration timeout) override;
  Result<Bytes> do_read(const ReadSpec& rs, Duration timeout) override;

  struct Io;
  TcpSettings settings_;
  std::unique_ptr<Io> io_;
};

}  // namespace pychron
