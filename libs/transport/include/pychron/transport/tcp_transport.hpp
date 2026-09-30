#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "pychron/transport/transport.hpp"

namespace pychron {

struct TcpSettings {
  std::string host;
  std::uint16_t port = 0;
};

// TCP client socket via asio. open() resolves and connects within the
// configured timeout.
class TcpTransport final : public QueuedTransport {
 public:
  TcpTransport(TcpSettings settings, TransportOptions options);
  ~TcpTransport() override;

  const TcpSettings& settings() const noexcept { return settings_; }

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
