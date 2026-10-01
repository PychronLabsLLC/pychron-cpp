#include "pychron/core/log_hub.hpp"

#include <spdlog/async_logger.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "log_hub_internal.hpp"
#include "pychron/core/log_match.hpp"
#include "pychron/core/logger.hpp"
#include "pychron/core/signal_bus.hpp"

namespace pychron {
namespace {

constexpr std::size_t kQueueSize = 8192;
constexpr std::size_t kWriterThreads = 1;
constexpr auto kPeriodicFlush = std::chrono::seconds(1);
constexpr auto kErrorReportInterval = std::chrono::seconds(10);
constexpr std::size_t kSpdlogMaxRotated = 200000;  // spdlog's rotating_file_sink::MaxFiles

spdlog::level::level_enum to_spdlog(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::Trace: return spdlog::level::trace;
    case LogLevel::Debug: return spdlog::level::debug;
    case LogLevel::Info: return spdlog::level::info;
    case LogLevel::Warn: return spdlog::level::warn;
    case LogLevel::Error: return spdlog::level::err;
  }
  return spdlog::level::err;
}

// "%*": our level names ("warn", not spdlog's "warning").
class LevelFlag final : public spdlog::custom_flag_formatter {
 public:
  void format(const spdlog::details::log_msg& msg, const std::tm&,
              spdlog::memory_buf_t& dest) override {
    std::string_view name = "error";
    switch (msg.level) {
      case spdlog::level::trace: name = "trace"; break;
      case spdlog::level::debug: name = "debug"; break;
      case spdlog::level::info: name = "info"; break;
      case spdlog::level::warn: name = "warn"; break;
      default: break;
    }
    dest.append(name.data(), name.data() + name.size());
  }
  std::unique_ptr<custom_flag_formatter> clone() const override {
    return std::make_unique<LevelFlag>();
  }
};

// `2026-10-01T14:03:22.481Z [warn] transport.serial.ig1: msg`; the payload
// already carries "<logger>: <message>".
std::unique_ptr<spdlog::formatter> make_file_formatter() {
  auto f = std::make_unique<spdlog::pattern_formatter>(spdlog::pattern_time_type::utc);
  f->add_flag<LevelFlag>('*').set_pattern("%Y-%m-%dT%H:%M:%S.%eZ [%*] %v");
  return f;
}

// Counts barrier records. The writer thread drains the queue in FIFO order, so
// once it reaches a barrier every record enqueued before it has been sunk.
class BarrierSink final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  void wait_for(std::uint64_t target) {
    std::unique_lock lock(m_);
    cv_.wait(lock, [&] { return reached_ >= target; });
  }

 protected:
  void sink_it_(const spdlog::details::log_msg&) override {
    {
      std::lock_guard lock(m_);
      ++reached_;
    }
    cv_.notify_all();
  }
  void flush_() override {}

 private:
  std::mutex m_;
  std::condition_variable cv_;
  std::uint64_t reached_ = 0;
};

}  // namespace

struct LogHub::Impl {
  const Clock* clock = nullptr;
  SignalBus* bus = nullptr;
  bool echo_stderr = false;
  std::mutex echo_mutex;

  // Level rules. `epoch` is bumped (under the unique lock) on every change;
  // hub loggers cache (level, epoch) and re-resolve when it moves.
  mutable std::shared_mutex rules_mutex;
  LogLevel default_level = LogLevel::Info;
  std::vector<std::pair<std::string, LogLevel>> rules;
  std::atomic<std::uint64_t> epoch{1};

  // Liveness: loggers hold this Impl and may outlive the LogHub. The
  // destructor clears `alive` and waits for in-flight writes to drain before
  // tearing the back end down; later writes are no-ops.
  std::atomic<bool> alive{true};
  std::atomic<int> in_flight{0};

  std::shared_ptr<spdlog::details::thread_pool> pool;
  std::shared_ptr<spdlog::sinks::rotating_file_sink_mt> file_sink;  // null = no file
  std::shared_ptr<spdlog::async_logger> logger;
  std::shared_ptr<BarrierSink> barrier_sink;
  std::shared_ptr<spdlog::async_logger> barrier;

  std::mutex barrier_mutex;  // orders barrier numbering with enqueueing
  std::uint64_t barriers_posted = 0;

  std::mutex flusher_mutex;
  std::condition_variable flusher_cv;
  bool stopping = false;
  std::thread flusher;

  std::atomic<std::int64_t> last_error_report{std::numeric_limits<std::int64_t>::min()};

  // spdlog error handler: one stderr line, at most once per 10 s.
  void report_spdlog_error(const std::string& what) noexcept {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    auto last = last_error_report.load();
    const auto interval =
        std::chrono::duration_cast<std::chrono::milliseconds>(kErrorReportInterval).count();
    if (last != std::numeric_limits<std::int64_t>::min() && now - last < interval) return;
    if (!last_error_report.compare_exchange_strong(last, now)) return;
    try {
      std::cerr << "pychron logging error: " << what << '\n';
    } catch (...) {
    }
  }

  void report_startup_error(const std::string& message) noexcept {
    try {
      if (bus != nullptr) {
        bus->publish(Log{LogLevel::Error, "logging", message, clock->now()});
      } else {
        std::cerr << "[error] logging: " << message << '\n';
      }
    } catch (...) {
    }
  }

  void put_rule(std::string_view pattern, LogLevel level) {
    for (auto& rule : rules) {
      if (rule.first == pattern) {
        rule.second = level;
        return;
      }
    }
    rules.emplace_back(std::string(pattern), level);
  }

  // Most specific matching rule; ties: longer pattern, then the
  // lexicographically greater one. Caller holds rules_mutex.
  LogLevel resolve_locked(std::string_view name) const noexcept {
    const std::pair<std::string, LogLevel>* best = nullptr;
    auto key = [](const std::string& p) {
      return std::make_tuple(log_rule_specificity(p), p.size(), std::string_view(p));
    };
    for (const auto& rule : rules) {
      if (!log_name_matches(rule.first, name)) continue;
      if (best == nullptr || key(rule.first) > key(best->first)) best = &rule;
    }
    return best != nullptr ? best->second : default_level;
  }

  // Runs entirely on the calling thread apart from the file enqueue, so bus
  // subscribers may log (even at error) without deadlocking the writer.
  void write(LogLevel level, std::string_view name, std::string_view message) noexcept {
    in_flight.fetch_add(1);
    struct Leave {
      std::atomic<int>& n;
      ~Leave() { n.fetch_sub(1); }
    } leave{in_flight};
    if (!alive.load()) return;

    try {
      std::string payload;
      payload.reserve(name.size() + 2 + message.size());
      payload.append(name).append(": ").append(message);
      // Raw string_view overload: the message is never a format string.
      logger->log(to_spdlog(level), spdlog::string_view_t(payload.data(), payload.size()));
      if (level >= LogLevel::Error) sync_flush();
    } catch (const std::exception& e) {
      report_spdlog_error(e.what());
    } catch (...) {
    }

    if (echo_stderr) {
      try {
        std::lock_guard lock(echo_mutex);
        std::cerr << '[' << to_string(level) << "] " << name << ": " << message << '\n';
      } catch (...) {
      }
    }

    if (bus != nullptr) {
      try {
        bus->publish(Log{level, std::string(name), std::string(message), clock->now()});
      } catch (...) {
      }
    }
  }

  void sync_flush() {
    std::uint64_t target = 0;
    {
      std::lock_guard lock(barrier_mutex);
      target = ++barriers_posted;
      barrier->log(spdlog::level::info, spdlog::string_view_t("barrier"));
    }
    barrier_sink->wait_for(target);
    if (file_sink) file_sink->flush();
  }

  void run_flusher() {
    std::unique_lock lock(flusher_mutex);
    while (!stopping) {
      flusher_cv.wait_for(lock, kPeriodicFlush, [&] { return stopping; });
      if (stopping) break;
      lock.unlock();
      logger->flush();  // asynchronous: queued behind pending records
      lock.lock();
    }
  }
};

Result<std::shared_ptr<LogHub>> LogHub::create(const config::LoggingConfig& config,
                                               const Clock& clock, SignalBus* bus) {
  auto impl = std::make_shared<Impl>();
  impl->clock = &clock;
  impl->bus = bus;
  impl->echo_stderr = config.echo_stderr;
  impl->default_level = config.default_level;
  for (const auto& [pattern, level] : config.levels) impl->put_rule(pattern, level);

  if (!config.dir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(config.dir, ec);
    if (ec) {
      impl->report_startup_error("cannot create log directory " + config.dir.string() + ": " +
                                 ec.message() + "; file logging disabled");
    } else {
      // Total files kept = max_files: spdlog counts rotated files only.
      const auto files = static_cast<std::size_t>(std::max<std::int64_t>(config.max_files, 1));
      const auto size_mb = static_cast<std::size_t>(std::max<std::int64_t>(config.max_size_mb, 1));
      try {
        impl->file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            (config.dir / "pychron.log").string(), size_mb * 1024 * 1024,
            std::min(files - 1, kSpdlogMaxRotated));
        impl->file_sink->set_formatter(make_file_formatter());
      } catch (const std::exception& e) {
        impl->file_sink.reset();
        impl->report_startup_error("cannot open log file in " + config.dir.string() + ": " +
                                   e.what() + "; file logging disabled");
      }
    }
  }

  try {
    impl->pool = std::make_shared<spdlog::details::thread_pool>(kQueueSize, kWriterThreads);
    std::vector<spdlog::sink_ptr> sinks;
    if (impl->file_sink) sinks.push_back(impl->file_sink);
    impl->logger = std::make_shared<spdlog::async_logger>(
        "pychron", sinks.begin(), sinks.end(), impl->pool, spdlog::async_overflow_policy::block);
    impl->logger->set_level(spdlog::level::trace);
    // Redundant with LogHub::write's synchronous flush, but keeps the writer
    // flushing error records even if a caller bypasses write().
    impl->logger->flush_on(spdlog::level::err);

    impl->barrier_sink = std::make_shared<BarrierSink>();
    impl->barrier = std::make_shared<spdlog::async_logger>(
        "pychron.barrier", impl->barrier_sink, impl->pool, spdlog::async_overflow_policy::block);
    impl->barrier->set_level(spdlog::level::trace);

    Impl* raw = impl.get();
    auto handler = [raw](const std::string& what) { raw->report_spdlog_error(what); };
    impl->logger->set_error_handler(handler);
    impl->barrier->set_error_handler(handler);

    impl->flusher = std::thread([raw] { raw->run_flusher(); });
  } catch (const std::exception& e) {
    return fail(ErrorKind::Io, std::string("cannot start logging: ") + e.what());
  }

  return std::shared_ptr<LogHub>(new LogHub(std::move(impl)));
}

LogHub::LogHub(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

LogHub::~LogHub() {
  // Hub loggers may still hold impl_: stop accepting writes and let the ones
  // in progress finish before the back end goes away.
  impl_->alive.store(false);
  while (impl_->in_flight.load() != 0) std::this_thread::yield();
  {
    std::lock_guard lock(impl_->flusher_mutex);
    impl_->stopping = true;
  }
  impl_->flusher_cv.notify_all();
  if (impl_->flusher.joinable()) impl_->flusher.join();
  try {
    impl_->sync_flush();
  } catch (...) {
  }
  // Loggers first; the pool's destructor drains whatever is still queued and
  // joins the writer thread.
  impl_->logger.reset();
  impl_->barrier.reset();
  impl_->pool.reset();
}

void LogHub::flush() {
  try {
    impl_->sync_flush();
  } catch (const std::exception& e) {
    impl_->report_spdlog_error(e.what());
  } catch (...) {
  }
}

void LogHub::write(LogLevel level, std::string_view logger, std::string_view message) {
  impl_->write(level, logger, message);
}

Logger LogHub::logger(std::string name) { return Logger(std::move(name), impl_); }

void LogHub::set_level(std::string_view pattern, LogLevel level) {
  std::unique_lock lock(impl_->rules_mutex);
  impl_->put_rule(pattern, level);
  impl_->epoch.fetch_add(1);
}

namespace detail {

std::uint64_t hub_rule_epoch(const LogHub::Impl& hub) noexcept { return hub.epoch.load(); }

ResolvedLevel hub_resolve_level(const LogHub::Impl& hub, std::string_view name) noexcept {
  try {
    std::shared_lock lock(hub.rules_mutex);
    return {hub.resolve_locked(name), hub.epoch.load()};
  } catch (...) {
    // Locking failed (system_error); fall back without caching a new epoch.
    return {LogLevel::Error, 0};
  }
}

void hub_write(LogHub::Impl& hub, LogLevel level, std::string_view logger,
               std::string_view message) noexcept {
  hub.write(level, logger, message);
}

}  // namespace detail

}  // namespace pychron
