#include "pychron/transport/udp_transport.hpp"

#include "asio_stream.hpp"

namespace pychron {

namespace {

// read_frame() speaks the stream vocabulary; a connected UDP socket receives
// one datagram per async_receive.
struct DatagramStream {
  asio::ip::udp::socket& socket;

  template <class Buffer, class Handler>
  void async_read_some(const Buffer& buffer, Handler&& handler) {
    socket.async_receive(buffer, std::forward<Handler>(handler));
  }
  void cancel(asio::error_code& ec) { socket.cancel(ec); }
};

}  // namespace

struct UdpTransport::Io {
  asio::io_context io;
  asio::ip::udp::socket socket{io};
  Bytes pending;
};

UdpTransport::UdpTransport(UdpSettings settings, TransportOptions options)
    : QueuedTransport(std::move(options)), settings_(std::move(settings)), io_(std::make_unique<Io>()) {}

UdpTransport::~UdpTransport() { shutdown(); }

Result<void> UdpTransport::do_open() {
  auto& io = *io_;
  const auto timeout = options().timeout;
  const auto target = settings_.host + ":" + std::to_string(settings_.port);

  asio::ip::udp::resolver resolver(io.io);
  bool done = false;
  asio::error_code ec;
  asio::ip::udp::resolver::results_type endpoints;
  resolver.async_resolve(settings_.host, std::to_string(settings_.port),
                         [&](const asio::error_code& e, asio::ip::udp::resolver::results_type r) {
                           ec = e;
                           endpoints = std::move(r);
                           done = true;
                         });
  if (detail::run_with_timeout(io.io, done, timeout, [&] { resolver.cancel(); }) && !done)
    return fail(ErrorKind::Io, "resolving " + target + " timed out");
  if (ec) return fail(detail::io_error("cannot resolve " + target, ec));
  if (endpoints.empty()) return fail(ErrorKind::Io, "cannot resolve " + target);

  // Connecting a UDP socket sends nothing: it fixes the peer for send and
  // filters what receive accepts.
  const auto peer = endpoints.begin()->endpoint();
  io.socket.open(peer.protocol(), ec);
  if (!ec) io.socket.connect(peer, ec);
  if (ec) {
    asio::error_code ignored;
    io.socket.close(ignored);
    return fail(detail::io_error("cannot open a UDP socket to " + target, ec));
  }
  io.pending.clear();
  return {};
}

void UdpTransport::do_close() {
  asio::error_code ignored;
  io_->socket.close(ignored);
  io_->pending.clear();
}

void UdpTransport::do_discard_input() {
  auto& io = *io_;
  io.pending.clear();
  asio::error_code ec;
  Bytes buf(kMaxDatagram);
  while (io.socket.is_open()) {
    const auto n = io.socket.available(ec);
    if (ec || n == 0) break;
    io.socket.receive(asio::buffer(buf), 0, ec);
    if (ec && ec != asio::error::connection_refused) break;
  }
}

Result<void> UdpTransport::do_write(const Bytes& tx, Duration timeout) {
  if (tx.size() > kMaxDatagram)
    return fail(ErrorKind::Config, "a " + std::to_string(tx.size()) + "-byte message does not fit one UDP datagram");
  auto& io = *io_;
  bool done = false;
  asio::error_code ec;
  std::size_t sent = 0;
  io.socket.async_send(asio::buffer(tx), [&](const asio::error_code& e, std::size_t n) {
    ec = e;
    sent = n;
    done = true;
  });
  const bool timed_out = detail::run_with_timeout(io.io, done, timeout, [&] {
    asio::error_code ignored;
    io.socket.cancel(ignored);
  });
  if (timed_out && (!ec || ec == asio::error::operation_aborted))
    return fail(ErrorKind::Timeout, "send not complete within " + detail::millis(timeout));
  if (ec) return fail(detail::io_error("send failed", ec));
  if (sent != tx.size()) return fail(ErrorKind::Io, "datagram sent short");
  return {};
}

Result<Bytes> UdpTransport::do_read(const ReadSpec& rs, Duration timeout) {
  DatagramStream stream{io_->socket};
  return detail::read_frame<kMaxDatagram>(io_->io, stream, io_->pending, rs, timeout);
}

}  // namespace pychron
