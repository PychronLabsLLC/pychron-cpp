#pragma once

#include <mutex>
#include <ostream>
#include <streambuf>
#include <string>
#include <string_view>

#include "pychron/core/logger.hpp"

namespace elctl {

// A streambuf that passes everything to `inner` and writes each complete line to `logger`:
// lines starting "error" or "elctl: " + "error" at error, every other line at warn.
//
// Unbuffered, as stderr is: every byte is handed to `inner` in the call that
// wrote it. Only the line not yet ended is kept, for the logger; it is logged
// when its newline comes, or when the tee goes. Empty lines make no record.
// Safe to write to from several threads. A record the logger itself writes to
// this stream while logging a line (an echo) is passed through, not logged
// again.
class LogTeeBuf : public std::streambuf {
 public:
  LogTeeBuf(std::streambuf* inner, const pychron::Logger& logger);
  ~LogTeeBuf() override;
  LogTeeBuf(const LogTeeBuf&) = delete;
  LogTeeBuf& operator=(const LogTeeBuf&) = delete;
  LogTeeBuf(LogTeeBuf&&) = delete;
  LogTeeBuf& operator=(LogTeeBuf&&) = delete;

 protected:
  int_type overflow(int_type ch) override;
  std::streamsize xsputn(const char* s, std::streamsize n) override;
  int sync() override;

 private:
  // Passes `s` on and logs the lines it completes.
  std::streamsize tee(const char* s, std::streamsize n);
  void log_line(std::string_view line) const noexcept;

  std::streambuf* inner_;
  pychron::Logger logger_;
  std::mutex mutex_;  // partial_, and the order bytes reach inner_
  std::string partial_;
};

// Puts a LogTeeBuf on `stream` (std::cerr) while it lives and gives the stream
// its own buffer back when it goes. Made after the process hub is set, and
// destroyed before it is unset.
class StreamTee {
 public:
  StreamTee(std::ostream& stream, const pychron::Logger& logger);
  ~StreamTee();
  StreamTee(const StreamTee&) = delete;
  StreamTee& operator=(const StreamTee&) = delete;
  StreamTee(StreamTee&&) = delete;
  StreamTee& operator=(StreamTee&&) = delete;

 private:
  std::ostream& stream_;
  std::streambuf* previous_;
  LogTeeBuf buf_;
};

}  // namespace elctl
