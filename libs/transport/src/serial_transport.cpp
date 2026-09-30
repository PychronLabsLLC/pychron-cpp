#include "pychron/transport/serial_transport.hpp"

#include "asio_stream.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <termios.h>
#endif

namespace pychron {

struct SerialTransport::Io {
  asio::io_context io;
  asio::serial_port port{io};
  Bytes pending;
};

SerialTransport::SerialTransport(SerialSettings settings, TransportOptions options)
    : QueuedTransport(std::move(options)), settings_(std::move(settings)), io_(std::make_unique<Io>()) {}

SerialTransport::~SerialTransport() { shutdown(); }

Result<void> SerialTransport::do_open() {
  using asio::serial_port_base;
  auto& port = io_->port;
  asio::error_code ec;
  port.open(settings_.port, ec);
  if (ec) return fail(detail::io_error("cannot open serial port '" + settings_.port + "'", ec));

  const auto parity = settings_.parity == SerialSettings::Parity::Even  ? serial_port_base::parity::even
                      : settings_.parity == SerialSettings::Parity::Odd ? serial_port_base::parity::odd
                                                                        : serial_port_base::parity::none;
  const auto stop = settings_.stop_bits == 2 ? serial_port_base::stop_bits::two : serial_port_base::stop_bits::one;

  auto configure = [&](const auto& option, const char* what) -> Result<void> {
    port.set_option(option, ec);
    if (!ec) return {};
    asio::error_code ignored;
    port.close(ignored);
    return fail(detail::io_error(std::string("cannot set ") + what + " on '" + settings_.port + "'", ec));
  };
  if (auto r = configure(serial_port_base::baud_rate(settings_.baud), "baud rate"); !r) return r;
  if (auto r = configure(serial_port_base::character_size(settings_.data_bits), "data bits"); !r) return r;
  if (auto r = configure(serial_port_base::parity(parity), "parity"); !r) return r;
  if (auto r = configure(serial_port_base::stop_bits(stop), "stop bits"); !r) return r;
  if (auto r = configure(serial_port_base::flow_control(serial_port_base::flow_control::none), "flow control"); !r)
    return r;
  io_->pending.clear();
  return {};
}

void SerialTransport::do_close() {
  asio::error_code ignored;
  io_->port.close(ignored);
  io_->pending.clear();
}

void SerialTransport::do_discard_input() {
  io_->pending.clear();
  if (!io_->port.is_open()) return;
#ifdef _WIN32
  ::PurgeComm(io_->port.native_handle(), PURGE_RXCLEAR);
#else
  ::tcflush(io_->port.native_handle(), TCIFLUSH);
#endif
}

Result<void> SerialTransport::do_write(const Bytes& tx, Duration timeout) {
  return detail::write_all(io_->io, io_->port, tx, timeout);
}

Result<Bytes> SerialTransport::do_read(const ReadSpec& rs, Duration timeout) {
  return detail::read_frame(io_->io, io_->port, io_->pending, rs, timeout);
}

}  // namespace pychron
