#include "pychron/transport/trace.hpp"

#include <charconv>
#include <fstream>
#include <istream>

namespace pychron {

std::string format_trace_record(const TraceRecord& record) {
  std::string out = std::to_string(record.at.count());
  switch (record.dir) {
    case TraceRecord::Dir::Tx: out += " tx " + to_hex(record.data); break;
    case TraceRecord::Dir::Rx: out += " rx " + to_hex(record.data); break;
    case TraceRecord::Dir::Err: out += " err " + record.message; break;
  }
  // An empty payload leaves a trailing space; trim it so lines stay tidy.
  if (out.back() == ' ') out.pop_back();
  return out;
}

namespace {

std::string_view trim(std::string_view s) {
  const auto ws = " \t\r";
  const auto b = s.find_first_not_of(ws);
  if (b == std::string_view::npos) return {};
  return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

// Splits off the first space-delimited token of `s`.
std::string_view next_token(std::string_view& s) {
  s = trim(s);
  const auto sp = s.find(' ');
  auto token = s.substr(0, sp);
  s = sp == std::string_view::npos ? std::string_view{} : s.substr(sp + 1);
  return token;
}

}  // namespace

Result<std::vector<TraceRecord>> parse_trace(std::istream& in) {
  std::vector<TraceRecord> records;
  std::string line;
  for (std::size_t line_no = 1; std::getline(in, line); ++line_no) {
    std::string_view rest = trim(line);
    if (rest.empty() || rest.front() == '#') continue;
    auto bad = [&](const std::string& why) {
      return fail(ErrorKind::Config, "trace line " + std::to_string(line_no) + ": " + why);
    };

    const auto time = next_token(rest);
    long long micros = 0;
    const auto [ptr, ec] = std::from_chars(time.data(), time.data() + time.size(), micros);
    if (ec != std::errc{} || ptr != time.data() + time.size()) return bad("bad timestamp '" + std::string(time) + "'");

    TraceRecord rec;
    rec.at = std::chrono::microseconds(micros);
    const auto dir = next_token(rest);
    if (dir == "tx" || dir == "rx") {
      rec.dir = dir == "tx" ? TraceRecord::Dir::Tx : TraceRecord::Dir::Rx;
      auto data = from_hex(trim(rest));
      if (!data) return bad("bad hex payload");
      rec.data = std::move(*data);
    } else if (dir == "err") {
      rec.dir = TraceRecord::Dir::Err;
      rec.message = std::string(trim(rest));
    } else {
      return bad("unknown direction '" + std::string(dir) + "'");
    }
    records.push_back(std::move(rec));
  }
  return records;
}

Result<std::vector<TraceRecord>> load_trace(const std::string& path) {
  std::ifstream in(path);
  if (!in) return fail(ErrorKind::Config, "cannot open trace file '" + path + "'");
  return parse_trace(in);
}

}  // namespace pychron
