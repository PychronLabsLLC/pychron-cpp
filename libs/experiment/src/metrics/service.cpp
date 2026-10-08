#include "pychron/experiment/metrics/service.hpp"

#include <optional>
#include <utility>

#include "pychron/core/events.hpp"
#include "pychron/experiment/metrics/experiment_metrics.hpp"
#include "pychron/metrics/core_exporter.hpp"
#include "pychron/metrics/scheduler_metrics.hpp"
#include "pychron/metrics/server.hpp"

namespace pychron::experiment::metrics {

// Declared in the order of dependence, so that destruction (the reverse)
// stops the endpoint first and frees the registry last.
struct MetricsService::Impl {
  Impl(SignalBus& b, const Clock& c, std::shared_ptr<LogHub> hub) : bus(b), clock(c) {
    if (hub) logger.emplace(hub->logger("metrics"));
  }

  void say(LogLevel level, const std::string& message) {
    if (logger) {
      logger->log(level, message);
    } else {
      bus.publish(Log{level, "metrics", message, clock.now()});
    }
  }

  SignalBus& bus;
  const Clock& clock;
  std::optional<Logger> logger;
  pychron::metrics::Registry registry;
  std::unique_ptr<pychron::metrics::CoreExporter> core;
  std::unique_ptr<ExperimentMetrics> experiment;
  std::unique_ptr<pychron::metrics::SchedulerMetrics> scheduler;
  std::unique_ptr<pychron::metrics::MetricsServer> server;
};

std::unique_ptr<MetricsService> MetricsService::start(const config::MetricsConfig& config, SignalBus& bus,
                                                      Scheduler& scheduler, const Clock& clock,
                                                      std::shared_ptr<LogHub> log_hub, std::string version) {
  if (!config.enabled) return nullptr;

  auto impl = std::make_unique<Impl>(bus, clock, std::move(log_hub));
  Impl* self = impl.get();
  impl->registry.on_family_full([self](std::string_view family) {
    self->say(LogLevel::Warn, "metrics: " + std::string(family) + " has reached its limit of " +
                                  std::to_string(pychron::metrics::Registry::kMaxSeriesPerFamily) +
                                  " series; further ones are not exported");
  });
  impl->core = std::make_unique<pychron::metrics::CoreExporter>(impl->registry, bus,
                                                               pychron::metrics::build_info(std::move(version)));
  impl->experiment = std::make_unique<ExperimentMetrics>(impl->registry, bus);
  impl->scheduler = std::make_unique<pychron::metrics::SchedulerMetrics>(impl->registry, scheduler);

  pychron::metrics::MetricsServer::Options options;
  options.bind = config.bind;
  options.port = static_cast<std::uint16_t>(config.port);
  auto server = pychron::metrics::MetricsServer::start(impl->registry, options);
  if (server) {
    impl->server = std::move(*server);
    impl->say(LogLevel::Info,
              "metrics: listening on " + config.bind + ":" + std::to_string(impl->server->port()));
  } else {
    const std::string message = "metrics endpoint is off: " + server.error().what;
    impl->say(LogLevel::Error, message);
    bus.publish(Alarm{"metrics", AlarmSeverity::Warning, message, clock.now()});
  }
  return std::unique_ptr<MetricsService>(new MetricsService(std::move(impl)));
}

MetricsService::MetricsService(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

MetricsService::~MetricsService() {
  // The full-family handler points into impl_; nothing may call it past here.
  impl_->registry.on_family_full({});
}

bool MetricsService::listening() const noexcept { return impl_->server != nullptr; }

std::uint16_t MetricsService::port() const noexcept { return impl_->server ? impl_->server->port() : 0; }

pychron::metrics::Registry& MetricsService::registry() noexcept { return impl_->registry; }

}  // namespace pychron::experiment::metrics
