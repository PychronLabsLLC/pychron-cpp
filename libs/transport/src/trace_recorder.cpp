#include "pychron/transport/trace_recorder.hpp"

#include <fstream>

namespace pychron {

namespace {

// "5B 50 52 31 0D 0A |PR1..|": byte count, uppercase hex, printable ASCII.
std::string wire_text(const Bytes& data) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out = std::to_string(data.size()) + "B";
  for (const auto b : data) {
    const auto v = static_cast<unsigned>(b);
    out += ' ';
    out += kHex[v >> 4];
    out += kHex[v & 0xF];
  }
  out += " |";
  for (const auto b : data) {
    const auto v = static_cast<unsigned>(b);
    out += (v >= 0x20 && v < 0x7F) ? static_cast<char>(v) : '.';
  }
  out += '|';
  return out;
}

}  // namespace

TraceRecorder::TraceRecorder(std::unique_ptr<Transport> inner, std::shared_ptr<std::ostream> sink,
                             const Clock& clock, std::optional<Logger> wire_log)
    : inner_(std::move(inner)),
      sink_(std::move(sink)),
      clock_(&clock),
      start_(clock.now()),
      wire_log_(std::move(wire_log)) {
  if (!sink_) return;
  *sink_ << "# trace of transport '" << inner_->name() << "'\n";
  sink_->flush();
}

Result<std::unique_ptr<TraceRecorder>> TraceRecorder::to_file(std::unique_ptr<Transport> inner,
                                                              const std::string& path, const Clock& clock,
                                                              std::optional<Logger> wire_log) {
  auto file = std::make_shared<std::ofstream>(path, std::ios::out | std::ios::trunc);
  if (!*file) return fail(ErrorKind::Io, "cannot open trace file '" + path + "' for writing", inner->name());
  return std::make_unique<TraceRecorder>(std::move(inner), std::move(file), clock, std::move(wire_log));
}

const std::string& TraceRecorder::name() const { return inner_->name(); }

Result<void> TraceRecorder::open() { return inner_->open(); }

void TraceRecorder::close() { inner_->close(); }

Health TraceRecorder::health() const { return inner_->health(); }

// Each call records inside the inner transport's own serialization (a
// transaction), so the file order is the wire order and the sink lock is
// never held while waiting for the bus. Holding a recorder-wide lock across
// the inner call would deadlock against a transaction running on the bus.

Result<Bytes> TraceRecorder::exchange(Bytes tx, ReadSpec rs, Duration timeout) {
  return transact(*inner_, [&]() -> Result<Bytes> {
    record(TraceRecord::Dir::Tx, tx);
    auto r = inner_->exchange(std::move(tx), std::move(rs), timeout);
    if (r) {
      record(TraceRecord::Dir::Rx, *r);
    } else {
      record_error(r.error());
    }
    return r;
  });
}

Result<void> TraceRecorder::write(Bytes tx) {
  return transact(*inner_, [&]() -> Result<void> {
    record(TraceRecord::Dir::Tx, tx);
    auto r = inner_->write(std::move(tx));
    if (!r) record_error(r.error());
    return r;
  });
}

Result<Bytes> TraceRecorder::read(ReadSpec rs, Duration timeout) {
  return transact(*inner_, [&]() -> Result<Bytes> {
    auto r = inner_->read(std::move(rs), timeout);
    if (r) {
      record(TraceRecord::Dir::Rx, *r);
    } else {
      record_error(r.error());
    }
    return r;
  });
}

Result<void> TraceRecorder::transaction(std::function<Result<void>()> body) {
  return inner_->transaction(std::move(body));
}

void TraceRecorder::record(TraceRecord::Dir dir, const Bytes& data) {
  std::lock_guard lock(mutex_);  // guards the sink only
  if (sink_) {
    const auto at = std::chrono::duration_cast<std::chrono::microseconds>(clock_->now() - start_);
    *sink_ << format_trace_record(TraceRecord{at, dir, data, {}}) << '\n';
    sink_->flush();
  }
  if (wire_log_ && wire_log_->enabled(LogLevel::Trace))
    wire_log_->trace(std::string(dir == TraceRecord::Dir::Tx ? "tx " : "rx ") + wire_text(data));
}

void TraceRecorder::record_error(const Error& error) {
  std::lock_guard lock(mutex_);  // guards the sink only
  if (sink_) {
    const auto at = std::chrono::duration_cast<std::chrono::microseconds>(clock_->now() - start_);
    *sink_ << format_trace_record(
                  TraceRecord{at, TraceRecord::Dir::Err, {}, std::string(to_string(error.kind)) + " " + error.what})
           << '\n';
    sink_->flush();
  }
  if (wire_log_ && wire_log_->enabled(LogLevel::Trace))
    wire_log_->trace("err " + std::string(to_string(error.kind)) + " " + error.what);
}

}  // namespace pychron
