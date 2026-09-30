#pragma once

#include <chrono>
#include <iosfwd>
#include <string>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/transport/bytes.hpp"

namespace pychron {

// One line of a trace file. Text format, one record per line:
//
//   # free-form comment
//   <micros> tx <hex>
//   <micros> rx <hex>
//   <micros> err <error kind> <message>
//
// <micros> is the offset since the recorder started. Blank lines and
// comments are ignored. Traces are committed under tests/traces/<vendor>/.
struct TraceRecord {
  enum class Dir { Tx, Rx, Err };

  std::chrono::microseconds at{};
  Dir dir = Dir::Tx;
  Bytes data;          // Tx / Rx
  std::string message; // Err: "<kind> <what>"

  friend bool operator==(const TraceRecord&, const TraceRecord&) = default;
};

std::string format_trace_record(const TraceRecord& record);

// Config error naming the offending line on malformed input.
Result<std::vector<TraceRecord>> parse_trace(std::istream& in);
Result<std::vector<TraceRecord>> load_trace(const std::string& path);

}  // namespace pychron
