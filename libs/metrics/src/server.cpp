#include "pychron/metrics/server.hpp"

#include <asio.hpp>
#include <atomic>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>

namespace pychron::metrics {

namespace {

using asio::ip::tcp;

struct Request {
  std::string_view method, target;
};

// "GET /metrics?x=1 HTTP/1.1" -> {GET, /metrics}. False for anything that is
// not an HTTP/1.x request line.
bool parse_request_line(std::string_view line, Request& out) {
  const auto sp1 = line.find(' ');
  if (sp1 == std::string_view::npos || sp1 == 0) return false;
  const auto sp2 = line.find(' ', sp1 + 1);
  if (sp2 == std::string_view::npos || sp2 == sp1 + 1) return false;
  const std::string_view version = line.substr(sp2 + 1);
  if (version != "HTTP/1.1" && version != "HTTP/1.0") return false;
  out.method = line.substr(0, sp1);
  for (const char c : out.method) {
    if (c < 'A' || c > 'Z') return false;
  }
  out.target = line.substr(sp1 + 1, sp2 - sp1 - 1);
  if (out.target.empty() || out.target[0] != '/') return false;
  out.target = out.target.substr(0, out.target.find('?'));
  return true;
}

std::string response(std::string_view status, std::string_view content_type, std::string_view body,
                     std::string_view extra_header = {}) {
  std::string r;
  r.reserve(body.size() + 160);
  r += "HTTP/1.1 ";
  r += status;
  r += "\r\nContent-Type: ";
  r += content_type;
  r += "\r\nContent-Length: " + std::to_string(body.size());
  r += "\r\nConnection: close\r\n";
  r += extra_header;
  r += "\r\n";
  r += body;
  return r;
}

constexpr std::string_view kMetricsType = "text/plain; version=0.0.4; charset=utf-8";
constexpr std::string_view kPlain = "text/plain; charset=utf-8";

}  // namespace

struct MetricsServer::Impl {
  Impl(Registry& r, Options o)
      : registry(r),
        options(std::move(o)),
        acceptor(io),
        scrape_seconds(r.gauge("pychron_metrics_scrape_duration_seconds",
                               "How long the last rendering of the registry took.")),
        bad_requests(r.counter("pychron_metrics_bad_requests_total",
                               "Connections dropped: too slow, too large, or not HTTP.")) {}

  // One client. Kept alive by its own handlers; when the last one is done
  // (or the io_context drops them at shutdown) the socket closes.
  struct Connection : std::enable_shared_from_this<Connection> {
    Connection(Impl& s, tcp::socket sock) : server(s), socket(std::move(sock)), timer(s.io), in(s.options.max_header_bytes) {
      server.live.insert(this);
    }
    ~Connection() { server.live.erase(this); }

    void run() {
      timer.expires_after(server.options.read_timeout);
      timer.async_wait([self = shared_from_this()](const asio::error_code& ec) {
        // Cancelled, or the request arrived while this was already on its
        // way: it is being answered and must not be cut short.
        if (ec || self->arrived) return;
        self->timed_out = true;
        asio::error_code ignored;
        self->socket.close(ignored);
      });
      asio::async_read_until(socket, in, "\r\n\r\n",
                             [self = shared_from_this()](const asio::error_code& ec, std::size_t) { self->on_headers(ec); });
    }

    void on_headers(const asio::error_code& ec) {
      arrived = true;
      timer.cancel();
      if (server.stopping) return;
      if (ec) {
        // The client left before saying anything: not its fault, not ours.
        const bool left_quietly = !timed_out && ec == asio::error::eof && in.size() == 0;
        if (!left_quietly) server.bad_requests.inc();
        return;
      }
      const auto data = in.data();
      const std::string_view text(static_cast<const char*>(data.data()), data.size());
      Request request;
      if (!parse_request_line(text.substr(0, text.find("\r\n")), request)) {
        server.bad_requests.inc();
        return;
      }
      out = server.answer(request);
      asio::async_write(socket, asio::buffer(out), [self = shared_from_this()](const asio::error_code&, std::size_t) {
        asio::error_code ignored;
        self->socket.shutdown(tcp::socket::shutdown_both, ignored);
      });
    }

    Impl& server;
    tcp::socket socket;
    asio::steady_timer timer;
    asio::streambuf in;
    std::string out;
    bool timed_out = false;
    bool arrived = false;  // the request is in: the deadline no longer applies
  };

  std::string answer(const Request& request) {
    if (request.method != "GET") return response("405 Method Not Allowed", kPlain, "GET only\n", "Allow: GET\r\n");
    if (request.target == "/metrics") {
      const auto began = std::chrono::steady_clock::now();
      std::string body = registry.render();
      scrape_seconds.set(std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count());
      return response("200 OK", kMetricsType, body);
    }
    if (request.target == "/healthz") return response("200 OK", kPlain, "ok\n");
    return response("404 Not Found", kPlain, "not found\n");
  }

  void accept() {
    acceptor.async_accept([this](const asio::error_code& ec, tcp::socket socket) {
      if (!acceptor.is_open()) return;
      if (!ec) {
        if (live.size() >= options.max_connections) {
          asio::error_code ignored;
          socket.close(ignored);  // a scrape is one client; this many is not a scrape
        } else {
          std::make_shared<Connection>(*this, std::move(socket))->run();
        }
      }
      accept();
    });
  }

  // The thread's whole life. Whatever goes wrong here costs the endpoint
  // and nothing else: an exception from a handler (no memory to render
  // with, an error asio throws) must not end the process.
  void serve() {
    for (;;) {
      try {
        io.run();
        return;  // out of work: the server is shutting down
      } catch (...) {
        // That request is lost; the rest go on.
      }
    }
  }

  Registry& registry;
  Options options;
  // Before `io`: the handlers `io` holds own the connections, and a
  // connection takes itself out of `live` when it goes.
  std::unordered_set<Connection*> live;  // the io thread only
  bool stopping = false;                 // the io thread only
  asio::io_context io;
  tcp::acceptor acceptor;
  Gauge& scrape_seconds;
  Counter& bad_requests;
  std::uint16_t port = 0;
  std::thread thread;
};

Result<std::unique_ptr<MetricsServer>> MetricsServer::start(Registry& registry, Options options) {
  asio::error_code ec;
  const asio::ip::address address = asio::ip::make_address(options.bind, ec);
  if (ec) return fail(ErrorKind::Config, "metrics: bind \"" + options.bind + "\" is not an IP address");

  auto impl = std::make_unique<Impl>(registry, std::move(options));
  const tcp::endpoint endpoint(address, impl->options.port);
  const std::string where = impl->options.bind + ":" + std::to_string(impl->options.port);
  const auto failed = [&where](const char* step, const asio::error_code& e) {
    return fail(ErrorKind::Io, std::string("metrics: cannot ") + step + " " + where + ": " + e.message());
  };

  impl->acceptor.open(endpoint.protocol(), ec);
  if (ec) return failed("open a socket for", ec);
#ifndef _WIN32
  // Lets a restart take the port again at once. Not on Windows, where the
  // same option lets a second process take a port that is in use.
  impl->acceptor.set_option(asio::socket_base::reuse_address(true), ec);
#endif
  impl->acceptor.bind(endpoint, ec);
  if (ec) return failed("bind", ec);
  impl->acceptor.listen(asio::socket_base::max_listen_connections, ec);
  if (ec) return failed("listen on", ec);
  impl->port = impl->acceptor.local_endpoint(ec).port();

  impl->accept();
  Impl* raw = impl.get();
  impl->thread = std::thread([raw] { raw->serve(); });
  return std::unique_ptr<MetricsServer>(new MetricsServer(std::move(impl)));
}

MetricsServer::MetricsServer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

MetricsServer::~MetricsServer() {
  // Closing everything on the io thread leaves it with nothing to wait for:
  // each pending handler runs once more, cancelled, and run() returns.
  asio::post(impl_->io, [impl = impl_.get()] {
    impl->stopping = true;
    asio::error_code ignored;
    impl->acceptor.close(ignored);
    for (Impl::Connection* c : impl->live) {
      c->timer.cancel();
      c->socket.close(ignored);
    }
  });
  if (impl_->thread.joinable()) impl_->thread.join();
}

std::uint16_t MetricsServer::port() const noexcept { return impl_->port; }

}  // namespace pychron::metrics
