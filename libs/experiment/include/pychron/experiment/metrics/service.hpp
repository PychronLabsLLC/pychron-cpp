#pragma once

// Everything `[metrics]` turns on, built and owned in one place: the
// registry, the exporters that fill it from the line's bus and scheduler, and
// the HTTP endpoint the lab's Prometheus scrapes.
//
// Metrics never stand in the application's way. When the endpoint cannot
// listen (the port is taken, the address is not this machine's) the service
// says so once, as an error in the log and a warning alarm, and the
// application runs on without it.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/config/metrics_config.hpp"
#include "pychron/core/log_hub.hpp"
#include "pychron/core/scheduler.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/metrics/registry.hpp"

namespace pychron {
class Link;
}

namespace pychron::experiment::metrics {

class MetricsService {
 public:
  // nullptr when `config.enabled` is false: nothing is built and no port is
  // opened. `bus`, `scheduler` and `clock` (the line's) must outlive the
  // service. Records go to `log_hub`'s "metrics" logger, or straight to the
  // bus without one. `version` is what pychron_build_info reports.
  static std::unique_ptr<MetricsService> start(const config::MetricsConfig& config, SignalBus& bus,
                                               Scheduler& scheduler, const Clock& clock,
                                               std::shared_ptr<LogHub> log_hub, std::string version);
  // Destroy it when nothing is publishing on the bus any more: after the
  // line has stopped, as the application does. The bus does not wait for a
  // handler that is mid-call when its subscription is dropped (signal_bus.hpp),
  // and such a handler would find the registry gone.
  ~MetricsService();
  MetricsService(const MetricsService&) = delete;
  MetricsService& operator=(const MetricsService&) = delete;

  bool listening() const noexcept;
  std::uint16_t port() const noexcept;  // 0 when not listening
  // Why it is not listening; empty when it is.
  const std::string& error() const noexcept;
  pychron::metrics::Registry& registry() noexcept;

  // The links whose reconnects and repeats each scrape reads
  // (pychron_link_reconnects_total, pychron_link_repeats_total), by their
  // transport's name: the line's and the spectrometer's. Each transport's
  // counters exist from this call. Replaces what an earlier call gave. The
  // links must outlive the service. A link given twice is read once.
  void watch_links(const std::vector<Link*>& links);

 private:
  struct Impl;
  explicit MetricsService(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace pychron::experiment::metrics
