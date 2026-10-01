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
#include <string>
#include <system_error>
#include <thread>
#include <vector>

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

  void report_startup_error(const std::string& message) {
    if (bus != nullptr) {
      bus->publish(Log{LogLevel::Error, "logging", message, clock->now()});
    } else {
      std::cerr << "[error] logging: " << message << '\n';
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
  auto impl = std::make_unique<Impl>();
  impl->clock = &clock;
  impl->bus = bus;

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

LogHub::LogHub(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

LogHub::~LogHub() {
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
  try {
    std::string payload;
    payload.reserve(logger.size() + 2 + message.size());
    payload.append(logger).append(": ").append(message);
    // Raw string_view overload: the message is never a format string.
    impl_->logger->log(to_spdlog(level), spdlog::string_view_t(payload.data(), payload.size()));
    if (level >= LogLevel::Error) impl_->sync_flush();
  } catch (const std::exception& e) {
    impl_->report_spdlog_error(e.what());
  } catch (...) {
  }
}

}  // namespace pychron
