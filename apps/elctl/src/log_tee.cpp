#include "log_tee.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace elctl {

namespace {

// Set while this thread logs a line of a tee: whatever the logger writes back
// to the stream then (its stderr echo) is passed through and not logged again.
thread_local bool t_logging = false;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): per-thread re-entry guard

}  // namespace

LogTeeBuf::LogTeeBuf(std::streambuf* inner, const pychron::Logger& logger) : inner_(inner), logger_(logger) {}

LogTeeBuf::~LogTeeBuf() {
  // Nothing else writes to a tee that is going.
  if (!partial_.empty()) log_line(partial_);
}

LogTeeBuf::int_type LogTeeBuf::overflow(int_type ch) {
  if (traits_type::eq_int_type(ch, traits_type::eof())) return traits_type::not_eof(ch);
  const char c = traits_type::to_char_type(ch);
  return tee(&c, 1) == 1 ? ch : traits_type::eof();
}

std::streamsize LogTeeBuf::xsputn(const char* s, std::streamsize n) { return tee(s, n); }

int LogTeeBuf::sync() { return inner_->pubsync(); }

std::streamsize LogTeeBuf::tee(const char* s, std::streamsize n) {
  if (t_logging) return inner_->sputn(s, n);
  std::vector<std::string> lines;
  std::streamsize passed = 0;
  {
    const std::lock_guard lock(mutex_);
    passed = inner_->sputn(s, n);
    const std::string_view bytes(s, static_cast<std::size_t>(std::max<std::streamsize>(passed, 0)));
    std::size_t begin = 0;
    for (auto end = bytes.find('\n'); end != std::string_view::npos; end = bytes.find('\n', begin)) {
      partial_.append(bytes.substr(begin, end - begin));
      lines.push_back(std::exchange(partial_, {}));
      begin = end + 1;
    }
    partial_.append(bytes.substr(begin));
  }
  // Logged outside the lock: a record may wait for the disk (error), and
  // another thread's bytes need not wait for it.
  for (const auto& line : lines) log_line(line);
  return passed;
}

void LogTeeBuf::log_line(std::string_view line) const noexcept {
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
  if (line.empty()) return;
  const bool error = line.starts_with("error") || line.starts_with("elctl: error");
  t_logging = true;
  try {
    logger_.log(error ? pychron::LogLevel::Error : pychron::LogLevel::Warn, line);
  } catch (...) {  // NOLINT(bugprone-empty-catch): the line is on stderr already; the log is best effort
  }
  t_logging = false;
}

StreamTee::StreamTee(std::ostream& stream, const pychron::Logger& logger)
    : stream_(stream), previous_(stream.rdbuf()), buf_(previous_, logger) {
  stream_.rdbuf(&buf_);
}

StreamTee::~StreamTee() { stream_.rdbuf(previous_); }

}  // namespace elctl
