#include "pychron/transport/tcp_transport.hpp"

#include "asio_stream.hpp"

#if defined(_WIN32)
#include <winsock2.h>
// winsock2 first: mstcpip needs its types.
#include <mstcpip.h>
#else
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

namespace pychron {

namespace {

[[maybe_unused]] constexpr int kKeepIdleSeconds = 10;
[[maybe_unused]] constexpr int kKeepIntervalSeconds = 5;
[[maybe_unused]] constexpr int kKeepCount = 3;

// Keepalive on, with the spec's timings where the platform takes them. A
// setting the platform refuses leaves its default: keepalive is a help in
// finding a dead peer, not a condition of being connected.
void enable_keepalive(asio::ip::tcp::socket& socket) {
  asio::error_code ignored;
  socket.set_option(asio::socket_base::keep_alive(true), ignored);
  auto fd = socket.native_handle();
#if defined(_WIN32)
  tcp_keepalive vals{};
  vals.onoff = 1;
  vals.keepalivetime = kKeepIdleSeconds * 1000;
  vals.keepaliveinterval = kKeepIntervalSeconds * 1000;
  DWORD returned = 0;
  (void)WSAIoctl(fd, SIO_KEEPALIVE_VALS, &vals, sizeof(vals), nullptr, 0, &returned, nullptr, nullptr);
#else
  const int idle = kKeepIdleSeconds;
#if defined(TCP_KEEPIDLE)
  (void)::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#elif defined(TCP_KEEPALIVE)
  (void)::setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof(idle));
#endif
#if defined(TCP_KEEPINTVL)
  const int interval = kKeepIntervalSeconds;
  (void)::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
#endif
#if defined(TCP_KEEPCNT)
  const int count = kKeepCount;
  (void)::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
#endif
#endif
}

std::optional<int> keepalive_idle_seconds([[maybe_unused]] asio::ip::tcp::socket& socket) {
#if !defined(_WIN32) && (defined(TCP_KEEPIDLE) || defined(TCP_KEEPALIVE))
  int idle = 0;
  socklen_t len = sizeof(idle);
#if defined(TCP_KEEPIDLE)
  const int option = TCP_KEEPIDLE;
#else
  const int option = TCP_KEEPALIVE;
#endif
  if (::getsockopt(socket.native_handle(), IPPROTO_TCP, option, &idle, &len) == 0) return idle;
#endif
  return std::nullopt;
}

}  // namespace

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
  enable_keepalive(io.socket);
  io.pending.clear();
  return {};
}

Result<TcpKeepalive> TcpTransport::keepalive() {
  TcpKeepalive out;
  auto r = transaction([&]() -> Result<void> {
    auto& socket = io_->socket;
    if (!socket.is_open()) return fail(ErrorKind::NotConnected, "transport is not open", name());
    asio::socket_base::keep_alive option;
    asio::error_code ec;
    socket.get_option(option, ec);
    if (ec) return fail(detail::io_error("cannot read keepalive", ec));
    out.on = option.value();
    out.idle_seconds = keepalive_idle_seconds(socket);
    return {};
  });
  if (!r) return fail(std::move(r).error());
  return out;
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
