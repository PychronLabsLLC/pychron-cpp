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
#include <csignal>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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
constexpr auto kCrashFlushTimeout = std::chrono::seconds(2);

// Set on the spdlog writer thread; the crash route must never wait on it from
// that thread (it would be waiting on itself).
thread_local bool t_is_log_writer = false;

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
  // Explicit "\n": spdlog's default end of line is "\r\n" on Windows.
  auto f = std::make_unique<spdlog::pattern_formatter>(spdlog::pattern_time_type::utc, "\n");
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
  bool wait_until(std::uint64_t target, std::chrono::steady_clock::time_point deadline) {
    std::unique_lock lock(m_);
    return cv_.wait_until(lock, deadline, [&] { return reached_ >= target; });
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

namespace {
// Defined with the crash handlers below; called from the flusher thread.
void refresh_crash_output(const LogHub::Impl& impl) noexcept;
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
  std::filesystem::path file_path;                                   // empty = no file
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

  // Terminate route: enqueue `text` at error and flush, waiting at most
  // kCrashFlushTimeout. Skipped on the writer thread, which cannot wait on
  // itself. Never throws.
  void crash_flush(std::string_view text) noexcept {
    in_flight.fetch_add(1);
    struct Leave {
      std::atomic<int>& n;
      ~Leave() { n.fetch_sub(1); }
    } leave{in_flight};
    if (!alive.load() || t_is_log_writer) return;

    try {
      std::string payload = "pychron: ";
      payload.append(text);
      logger->log(spdlog::level::err, spdlog::string_view_t(payload.data(), payload.size()));
      if (echo_stderr) {
        std::lock_guard lock(echo_mutex);
        std::cerr << "[error] " << payload << '\n';
      }

      const auto deadline = std::chrono::steady_clock::now() + kCrashFlushTimeout;
      std::unique_lock lock(barrier_mutex, std::defer_lock);
      while (!lock.try_lock()) {  // the crashing thread may already hold it
        if (std::chrono::steady_clock::now() >= deadline) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      const std::uint64_t target = ++barriers_posted;
      barrier->log(spdlog::level::info, spdlog::string_view_t("barrier"));
      lock.unlock();
      if (!barrier_sink->wait_until(target, deadline)) return;
      if (file_sink) file_sink->flush();
    } catch (...) {
    }
  }

  void run_flusher() {
    std::unique_lock lock(flusher_mutex);
    while (!stopping) {
      flusher_cv.wait_for(lock, kPeriodicFlush, [&] { return stopping; });
      if (stopping) break;
      lock.unlock();
      logger->flush();  // asynchronous: queued behind pending records
      refresh_crash_output(*this);  // follow pychron.log across rotation
      lock.lock();
    }
  }
};

namespace {

// ---- Crash handlers (spec 4.5) --------------------------------------------

#ifdef _WIN32
using CrashTarget = HANDLE;
const CrashTarget kNoCrashTarget = INVALID_HANDLE_VALUE;
#else
using CrashTarget = int;
constexpr CrashTarget kNoCrashTarget = -1;
#endif

// Descriptor on the registered hub's pychron.log; read lock-free by the
// signal/exception handler.
std::atomic<CrashTarget> g_crash_target{kNoCrashTarget};
std::atomic<std::terminate_handler> g_previous_terminate{nullptr};

// Normal-context state. Leaked so it survives static destruction.
struct CrashState {
  std::mutex mutex;
  std::weak_ptr<LogHub::Impl> current;  // most recently created hub
  bool installed = false;
};
CrashState& crash_state() {
  static auto* state = new CrashState;
  return *state;
}

CrashTarget open_crash_target(const std::filesystem::path& path) noexcept {
#ifdef _WIN32
  return CreateFileW(path.c_str(), FILE_APPEND_DATA,
                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                     FILE_ATTRIBUTE_NORMAL, nullptr);
#else
  return ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
#endif
}

void close_crash_target(CrashTarget target) noexcept {
  if (target == kNoCrashTarget) return;
#ifdef _WIN32
  CloseHandle(target);
#else
  ::close(target);
#endif
}

// Points the handler at `impl`'s log file. Caller holds CrashState::mutex.
//
// POSIX: the handler's descriptor number never changes once set. A new file
// is opened and dup2()ed onto it, which swaps the open file atomically with
// respect to a handler running concurrently, so no descriptor the handler may
// be using is ever closed.
void retarget_crash_output(const LogHub::Impl& impl) noexcept {
  if (impl.file_path.empty()) return;
  const CrashTarget fresh = open_crash_target(impl.file_path);
  if (fresh == kNoCrashTarget) return;
#ifdef _WIN32
  close_crash_target(g_crash_target.exchange(fresh));
#else
  const CrashTarget current = g_crash_target.load();
  if (current == kNoCrashTarget) {
    g_crash_target.store(fresh);
    return;
  }
  ::dup2(fresh, current);  // dup2 clears FD_CLOEXEC on `current`; restore it
  ::fcntl(current, F_SETFD, FD_CLOEXEC);
  close_crash_target(fresh);
#endif
}

// Flusher tick: if `impl` is the hub the handlers write to and its
// pychron.log has been replaced (rotation), retarget onto the live file.
void refresh_crash_output(const LogHub::Impl& impl) noexcept {
#ifdef _WIN32
  (void)impl;  // the handle is only (re)opened when a hub is created
#else
  if (impl.file_path.empty()) return;
  auto& state = crash_state();
  std::unique_lock lock(state.mutex, std::try_to_lock);
  if (!lock.owns_lock() || !state.installed) return;
  if (state.current.lock().get() != &impl) return;
  const CrashTarget current = g_crash_target.load();
  struct stat live {};
  struct stat open_file {};
  if (::stat(impl.file_path.c_str(), &live) != 0) return;  // mid-rotation; next tick
  if (current != kNoCrashTarget && ::fstat(current, &open_file) == 0 &&
      live.st_dev == open_file.st_dev && live.st_ino == open_file.st_ino)
    return;
  retarget_crash_output(impl);
#endif
}

// Async-signal-safe write of a whole buffer; errors are ignored.
void write_all(CrashTarget target, const char* data, std::size_t size) noexcept {
  if (target == kNoCrashTarget) return;
#ifdef _WIN32
  DWORD written = 0;
  WriteFile(target, data, static_cast<DWORD>(size), &written, nullptr);
#else
  while (size > 0) {
    const ssize_t n = ::write(target, data, size);
    if (n <= 0) return;
    data += n;
    size -= static_cast<std::size_t>(n);
  }
#endif
}

// Appends the decimal (or, with base 16, hex) digits of `value` to buf at pos.
std::size_t put_unsigned(char* buf, std::size_t pos, unsigned long value, unsigned base) noexcept {
  char digits[24];
  std::size_t n = 0;
  do {
    digits[n++] = "0123456789abcdef"[value % base];
    value /= base;
  } while (value != 0 && n < sizeof digits);
  while (n > 0) buf[pos++] = digits[--n];
  return pos;
}

std::string terminate_text() {
  const auto active = std::current_exception();
  if (!active) return "terminate called without an active exception";
  try {
    // cppcheck-suppress missingReturn ; std::rethrow_exception does not return
    std::rethrow_exception(active);
  } catch (const std::exception& e) {
    return std::string("terminate called after throwing: ") + e.what();
  } catch (...) {
    return "terminate called after throwing a non-std::exception";
  }
}

[[noreturn]] void on_terminate() noexcept {
  std::shared_ptr<LogHub::Impl> hub;
  {
    // Bounded: terminate may fire on a thread that already holds the mutex.
    auto& state = crash_state();
    std::unique_lock lock(state.mutex, std::defer_lock);
    for (int i = 0; i < 100 && !lock.try_lock(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (lock.owns_lock()) hub = state.current.lock();
  }
  if (hub) {
    try {
      hub->crash_flush(terminate_text());
    } catch (...) {
    }
  }
  hub.reset();
  if (const auto previous = g_previous_terminate.load()) previous();
  std::abort();
}

#ifdef _WIN32

LONG WINAPI on_unhandled_exception(EXCEPTION_POINTERS* info) {
  static const char kPrefix[] = "fatal exception 0x";
  char line[64];
  std::size_t pos = 0;
  for (const char c : std::string_view(kPrefix)) line[pos++] = c;
  const auto code = (info != nullptr && info->ExceptionRecord != nullptr)
                        ? static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode)
                        : 0UL;
  pos = put_unsigned(line, pos, code, 16);
  line[pos++] = '\n';
  write_all(GetStdHandle(STD_ERROR_HANDLE), line, pos);
  write_all(g_crash_target.load(), line, pos);
  return EXCEPTION_CONTINUE_SEARCH;
}

void install_fatal_handlers() noexcept { SetUnhandledExceptionFilter(&on_unhandled_exception); }

#else

// "fatal signal N\n", preformatted at install time so the handler only
// copies bytes.
struct SignalLine {
  int signal = 0;
  char text[32] = {};
  std::size_t size = 0;
};
constexpr int kFatalSignals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL};
SignalLine g_signal_lines[std::size(kFatalSignals)];

void on_fatal_signal(int sig) {
  for (const auto& line : g_signal_lines) {
    if (line.signal != sig) continue;
    write_all(STDERR_FILENO, line.text, line.size);
    write_all(g_crash_target.load(), line.text, line.size);
    break;
  }
  std::signal(sig, SIG_DFL);
  std::raise(sig);  // delivered with the default action once this returns
}

void install_fatal_handlers() noexcept {
  static constexpr std::string_view kPrefix = "fatal signal ";
  for (std::size_t i = 0; i < std::size(kFatalSignals); ++i) {
    auto& line = g_signal_lines[i];
    line.signal = kFatalSignals[i];
    std::size_t pos = 0;
    for (const char c : kPrefix) line.text[pos++] = c;
    pos = put_unsigned(line.text, pos, static_cast<unsigned long>(line.signal), 10);
    line.text[pos++] = '\n';
    line.size = pos;
  }
  for (const int sig : kFatalSignals) {
    struct sigaction action {};
    action.sa_handler = &on_fatal_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(sig, &action, nullptr);
  }
}

#endif

}  // namespace

void LogHub::install_crash_handlers() {
  auto& state = crash_state();
  std::lock_guard lock(state.mutex);
  if (state.installed) return;
  state.installed = true;
  if (auto hub = state.current.lock()) retarget_crash_output(*hub);
  install_fatal_handlers();
  g_previous_terminate.store(std::set_terminate(&on_terminate));
}

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
        impl->file_path = config.dir / "pychron.log";
      } catch (const std::exception& e) {
        impl->file_sink.reset();
        impl->report_startup_error("cannot open log file in " + config.dir.string() + ": " +
                                   e.what() + "; file logging disabled");
      }
    }
  }

  try {
    impl->pool = std::make_shared<spdlog::details::thread_pool>(
        kQueueSize, kWriterThreads, [] { t_is_log_writer = true; });
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

  {
    auto& state = crash_state();
    std::lock_guard lock(state.mutex);
    state.current = impl;
    if (state.installed) retarget_crash_output(*impl);
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
  impl_->file_sink.reset();
  impl_->barrier_sink.reset();
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

SignalBus* LogHub::bus() const noexcept { return impl_->bus; }

void LogHub::set_level(std::string_view pattern, LogLevel level) {
  std::unique_lock lock(impl_->rules_mutex);
  impl_->put_rule(pattern, level);
  impl_->epoch.fetch_add(1);
}

void LogHub::set_levels(LogLevel default_level, const std::vector<std::pair<std::string, LogLevel>>& levels) {
  std::unique_lock lock(impl_->rules_mutex);
  impl_->default_level = default_level;
  impl_->rules.clear();
  for (const auto& [pattern, level] : levels) impl_->put_rule(pattern, level);
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
