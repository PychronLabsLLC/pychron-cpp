#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "pychron/transport/transport.hpp"

namespace pychron {

struct SerialSettings {
  enum class Parity { None, Even, Odd };

  std::string port;  // "/dev/tty.usbserial-A1" or "COM4"
  std::uint32_t baud = 9600;
  unsigned data_bits = 8;
  unsigned stop_bits = 1;  // 1 or 2
  Parity parity = Parity::None;
};

// RS-232/RS-485 port via asio::serial_port.
class SerialTransport final : public QueuedTransport {
 public:
  SerialTransport(SerialSettings settings, TransportOptions options);
  ~SerialTransport() override;

  const SerialSettings& settings() const noexcept { return settings_; }

 private:
  Result<void> do_open() override;
  void do_close() override;
  void do_discard_input() override;
  Result<void> do_write(const Bytes& tx, Duration timeout) override;
  Result<Bytes> do_read(const ReadSpec& rs, Duration timeout) override;

  struct Io;
  SerialSettings settings_;
  std::unique_ptr<Io> io_;
};

}  // namespace pychron
