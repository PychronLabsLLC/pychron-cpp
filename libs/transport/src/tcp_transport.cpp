#include "pychron/transport/tcp_transport.hpp"

#include "asio_stream.hpp"

namespace pychron {

struct TcpTransport::Io {
  asio::io_context io;
  asio::ip::tcp::socket socket{io};
  Bytes pending;
};

TcpTransport::TcpTransport(TcpSettings settings, TransportOptions options)
    : QueuedTransport(std::move(options)), settings_(std::move(settings)), io_(std::make_unique<Io>()) {}

TcpTransport::~TcpTransport() { shutdown(); }

Result<void> TcpTransport::do_open() {
  auto& io = *io_;
  const auto timeout = options().timeout;
  const auto target = settings_.host + ":" + std::to_string(settings_.port);

  asio::ip::tcp::resolver resolver(io.io);
  bool done = false;
  asio::error_code ec;
  asio::ip::tcp::resolver::results_type endpoints;
  resolver.async_resolve(settings_.host, std::to_string(settings_.port),
                         [&](const asio::error_code& e, asio::ip::tcp::resolver::results_type r) {
                           ec = e;
                           endpoints = std::move(r);
                           done = true;
                         });
  if (detail::run_with_timeout(io.io, done, timeout, [&] { resolver.cancel(); }) && !done)
    return fail(ErrorKind::Io, "resolving " + target + " timed out");
  if (ec) return fail(detail::io_error("cannot resolve " + target, ec));

  done = false;
  asio::async_connect(io.socket, endpoints, [&](const asio::error_code& e, const asio::ip::tcp::endpoint&) {
    ec = e;
    done = true;
  });
  const bool timed_out = detail::run_with_timeout(io.io, done, timeout, [&] {
    asio::error_code ignored;
    io.socket.close(ignored);
  });
  if (timed_out && (!ec || ec == asio::error::operation_aborted))
    return fail(ErrorKind::Io, "connecting to " + target + " timed out after " + detail::millis(timeout));
  if (ec) {
    asio::error_code ignored;
    io.socket.close(ignored);
    return fail(detail::io_error("cannot connect to " + target, ec));
  }
  asio::error_code ignored;
  io.socket.set_option(asio::ip::tcp::no_delay(true), ignored);
  io.pending.clear();
  return {};
}

void TcpTransport::do_close() {
  asio::error_code ignored;
  io_->socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
  io_->socket.close(ignored);
  io_->pending.clear();
}

void TcpTransport::do_discard_input() {
  auto& io = *io_;
  io.pending.clear();
  asio::error_code ec;
  std::array<std::uint8_t, 512> buf{};
  while (io.socket.is_open()) {
    const auto n = io.socket.available(ec);
    if (ec || n == 0) break;
    io.socket.read_some(asio::buffer(buf, std::min(n, buf.size())), ec);
    if (ec) break;
  }
}

Result<void> TcpTransport::do_write(const Bytes& tx, Duration timeout) {
  return detail::write_all(io_->io, io_->socket, tx, timeout);
}

Result<Bytes> TcpTransport::do_read(const ReadSpec& rs, Duration timeout) {
  return detail::read_frame(io_->io, io_->socket, io_->pending, rs, timeout);
}

}  // namespace pychron
