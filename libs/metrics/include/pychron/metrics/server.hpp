#pragma once

// The HTTP endpoint a Prometheus server scrapes.
//
//   GET /metrics   the registry, text format 0.0.4
//   GET /healthz   "ok"
//
// Read-only, plain HTTP, one request per connection. It runs on a thread of
// its own, in real time: it is not a participant in the line's Clock and
// shares nothing with the scheduler but the registry. A client that is slow,
// sends too much or sends something that is not HTTP is dropped and counted;
// nothing a client does is logged.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "pychron/core/error.hpp"
#include "pychron/metrics/registry.hpp"

namespace pychron::metrics {

class MetricsServer {
 public:
  struct Options {
    std::string bind = "0.0.0.0";  // an IPv4 or IPv6 address
    std::uint16_t port = 9464;     // 0: the OS picks one (see port())
    std::chrono::milliseconds read_timeout{5000};  // for the whole request to arrive
    std::size_t max_header_bytes = 8 * 1024;
    std::size_t max_connections = 8;
  };

  // Listening when it returns. A `bind` that is not an address is a Config
  // error; an address or port that cannot be had (in use, not this
  // machine's) is an Io error. `registry` must outlive the server.
  static Result<std::unique_ptr<MetricsServer>> start(Registry& registry, Options options);

  // Closes every connection and joins the thread.
  ~MetricsServer();
  MetricsServer(const MetricsServer&) = delete;
  MetricsServer& operator=(const MetricsServer&) = delete;

  std::uint16_t port() const noexcept;

 private:
  struct Impl;
  explicit MetricsServer(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace pychron::metrics
