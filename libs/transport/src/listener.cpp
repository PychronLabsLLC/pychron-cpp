#include "pychron/transport/listener.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>

#include "asio_stream.hpp"
#include "pychron/transport/udp_transport.hpp"

namespace pychron {

namespace {

using namespace std::chrono_literals;

// The longest a wait goes without looking at the closed flag: close() from
// another thread may not touch the socket the owner is waiting on.
constexpr Duration kSlice = 50ms;

Unexpected<Error> cancelled() { return fail(ErrorKind::Cancelled, "closed"); }

// Runs one started async op for up to `slice`, cancelling it with `cancel`
// on timeout. True when its handler ran (successfully or not).
template <class Cancel>
bool run_slice(asio::io_context& io, const bool& done, Duration slice, Cancel&& cancel) {
  detail::run_with_timeout(io, done, slice, std::forward<Cancel>(cancel));
  return done;
}

Result<asio::ip::address> parse_address(const std::string& address, std::uint16_t port) {
  asio::error_code ec;
  auto parsed = asio::ip::make_address(address, ec);
  if (ec) return fail(detail::io_error("cannot bind " + address + ":" + std::to_string(port), ec));
  return parsed;
}

class TcpStream final : public ByteStream {
 public:
  TcpStream() = default;
  ~TcpStream() override {
    asio::error_code ignored;
    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);
  }
  TcpStream(const TcpStream&) = delete;
  TcpStream& operator=(const TcpStream&) = delete;

  asio::ip::tcp::socket& socket() noexcept { return socket_; }

  Result<Bytes> read_some(Duration timeout) override {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::array<std::uint8_t, 4096> buf{};
    for (;;) {
      if (closed_) return cancelled();
      bool done = false;
      asio::error_code ec;
      std::size_t got = 0;
      socket_.async_read_some(asio::buffer(buf), [&](const asio::error_code& e, std::size_t n) {
        ec = e;
        got = n;
        done = true;
      });
      const auto left = deadline - std::chrono::steady_clock::now();
      run_slice(io_, done, std::clamp(left, Duration::zero(), kSlice), [&] {
        asio::error_code ignored;
        socket_.cancel(ignored);
      });
      if (got > 0) return Bytes(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(got));
      if (ec == asio::error::eof) return fail(ErrorKind::Io, "connection closed by peer");
      if (ec && ec != asio::error::operation_aborted) return fail(detail::io_error("read failed", ec));
      if (std::chrono::steady_clock::now() >= deadline) return Bytes{};
    }
  }

  Result<void> write(const Bytes& bytes, Duration timeout) override {
    if (closed_) return cancelled();
    return detail::write_all(io_, socket_, bytes, timeout);
  }

  void close() override { closed_ = true; }

 private:
  asio::io_context io_;
  asio::ip::tcp::socket socket_{io_};
  std::atomic<bool> closed_{false};
};

}  // namespace

// --- TcpListener -------------------------------------------------------------

struct TcpListener::Io {
  asio::io_context io;
  asio::ip::tcp::acceptor acceptor{io};
  std::uint16_t port = 0;
  std::atomic<bool> closed{false};
};

TcpListener::TcpListener(std::unique_ptr<Io> io) : io_(std::move(io)) {}

TcpListener::~TcpListener() {
  asio::error_code ignored;
  io_->acceptor.close(ignored);
}

Result<std::unique_ptr<TcpListener>> TcpListener::bind(const std::string& address, std::uint16_t port) {
  auto parsed = parse_address(address, port);
  if (!parsed) return fail(parsed.error());
  auto io = std::make_unique<Io>();
  const asio::ip::tcp::endpoint endpoint(*parsed, port);
  const std::string where = address + ":" + std::to_string(port);
  asio::error_code ec;
  io->acceptor.open(endpoint.protocol(), ec);
  // Not SO_REUSEADDR: on Windows it lets a second listener take a bound port.
  if (!ec) io->acceptor.bind(endpoint, ec);
  if (!ec) io->acceptor.listen(asio::socket_base::max_listen_connections, ec);
  if (ec) return fail(detail::io_error("cannot bind " + where, ec));
  io->port = io->acceptor.local_endpoint(ec).port();
  if (ec) return fail(detail::io_error("cannot read the port bound at " + where, ec));
  return std::unique_ptr<TcpListener>(new TcpListener(std::move(io)));
}

std::uint16_t TcpListener::port() const noexcept { return io_->port; }

Result<std::unique_ptr<ByteStream>> TcpListener::accept(Duration timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    if (io_->closed) return cancelled();
    auto stream = std::make_unique<TcpStream>();
    bool done = false;
    asio::error_code ec;
    io_->acceptor.async_accept(stream->socket(), [&](const asio::error_code& e) {
      ec = e;
      done = true;
    });
    const auto left = deadline - std::chrono::steady_clock::now();
    run_slice(io_->io, done, std::clamp(left, Duration::zero(), kSlice), [&] {
      asio::error_code ignored;
      io_->acceptor.cancel(ignored);
    });
    // A connection accepted as the wait was cancelled is still a connection.
    if (done && !ec) {
      asio::error_code ignored;
      stream->socket().set_option(asio::ip::tcp::no_delay(true), ignored);
      return std::unique_ptr<ByteStream>(std::move(stream));
    }
    if (ec && ec != asio::error::operation_aborted) return fail(detail::io_error("accept failed", ec));
    if (std::chrono::steady_clock::now() >= deadline) {
      return fail(ErrorKind::Timeout, "no connection within " + detail::millis(timeout));
    }
  }
}

void TcpListener::close() { io_->closed = true; }

// --- UdpListener -------------------------------------------------------------

struct UdpListener::Peer {
  asio::ip::udp::endpoint endpoint;
};

struct UdpListener::Io {
  asio::io_context io;
  asio::ip::udp::socket socket{io};
  std::uint16_t port = 0;
  std::atomic<bool> closed{false};
};

UdpListener::UdpListener(std::unique_ptr<Io> io) : io_(std::move(io)) {}

UdpListener::~UdpListener() {
  asio::error_code ignored;
  io_->socket.close(ignored);
}

Result<std::unique_ptr<UdpListener>> UdpListener::bind(const std::string& address, std::uint16_t port) {
  auto parsed = parse_address(address, port);
  if (!parsed) return fail(parsed.error());
  auto io = std::make_unique<Io>();
  const asio::ip::udp::endpoint endpoint(*parsed, port);
  const std::string where = address + ":" + std::to_string(port);
  asio::error_code ec;
  io->socket.open(endpoint.protocol(), ec);
  if (!ec) io->socket.bind(endpoint, ec);
  if (ec) return fail(detail::io_error("cannot bind " + where, ec));
  io->port = io->socket.local_endpoint(ec).port();
  if (ec) return fail(detail::io_error("cannot read the port bound at " + where, ec));
  return std::unique_ptr<UdpListener>(new UdpListener(std::move(io)));
}

std::uint16_t UdpListener::port() const noexcept { return io_->port; }

Result<UdpListener::Datagram> UdpListener::receive(Duration timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  Bytes buf(UdpTransport::kMaxDatagram);
  for (;;) {
    if (io_->closed) return cancelled();
    auto from = std::make_shared<Peer>();
    bool done = false;
    asio::error_code ec;
    std::size_t got = 0;
    io_->socket.async_receive_from(asio::buffer(buf), from->endpoint,
                                   [&](const asio::error_code& e, std::size_t n) {
                                     ec = e;
                                     got = n;
                                     done = true;
                                   });
    const auto left = deadline - std::chrono::steady_clock::now();
    run_slice(io_->io, done, std::clamp(left, Duration::zero(), kSlice), [&] {
      asio::error_code ignored;
      io_->socket.cancel(ignored);
    });
    if (done && !ec) {
      buf.resize(got);
      return Datagram{std::move(buf), std::move(from)};
    }
    // A reply that reached a closed port comes back as an ICMP error on some
    // platforms: not this listener's failure.
    if (ec && ec != asio::error::operation_aborted && ec != asio::error::connection_refused &&
        ec != asio::error::connection_reset) {
      return fail(detail::io_error("receive failed", ec));
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return fail(ErrorKind::Timeout, "no datagram within " + detail::millis(timeout));
    }
  }
}

Result<void> UdpListener::send_to(const Peer& to, const Bytes& bytes) {
  if (io_->closed) return cancelled();
  asio::error_code ec;
  io_->socket.send_to(asio::buffer(bytes), to.endpoint, 0, ec);
  if (ec) return fail(detail::io_error("send failed", ec));
  return {};
}

void UdpListener::close() { io_->closed = true; }

}  // namespace pychron
