#include "pychron/transport/trace_recorder.hpp"

#include <fstream>

namespace pychron {

TraceRecorder::TraceRecorder(std::unique_ptr<Transport> inner, std::shared_ptr<std::ostream> sink,
                             const Clock& clock)
    : inner_(std::move(inner)), sink_(std::move(sink)), clock_(&clock), start_(clock.now()) {
  *sink_ << "# trace of transport '" << inner_->name() << "'\n";
  sink_->flush();
}

Result<std::unique_ptr<TraceRecorder>> TraceRecorder::to_file(std::unique_ptr<Transport> inner,
                                                              const std::string& path, const Clock& clock) {
  auto file = std::make_shared<std::ofstream>(path, std::ios::out | std::ios::trunc);
  if (!*file) return fail(ErrorKind::Io, "cannot open trace file '" + path + "' for writing", inner->name());
  return std::make_unique<TraceRecorder>(std::move(inner), std::move(file), clock);
}

const std::string& TraceRecorder::name() const { return inner_->name(); }

Result<void> TraceRecorder::open() { return inner_->open(); }

void TraceRecorder::close() { inner_->close(); }

Health TraceRecorder::health() const { return inner_->health(); }

Result<Bytes> TraceRecorder::exchange(Bytes tx, ReadSpec rs, Duration timeout) {
  std::lock_guard lock(mutex_);
  record(TraceRecord::Dir::Tx, tx);
  auto r = inner_->exchange(std::move(tx), std::move(rs), timeout);
  if (r) {
    record(TraceRecord::Dir::Rx, *r);
  } else {
    record_error(r.error());
  }
  return r;
}

Result<void> TraceRecorder::write(Bytes tx) {
  std::lock_guard lock(mutex_);
  record(TraceRecord::Dir::Tx, tx);
  auto r = inner_->write(std::move(tx));
  if (!r) record_error(r.error());
  return r;
}

Result<Bytes> TraceRecorder::read(ReadSpec rs, Duration timeout) {
  std::lock_guard lock(mutex_);
  auto r = inner_->read(std::move(rs), timeout);
  if (r) {
    record(TraceRecord::Dir::Rx, *r);
  } else {
    record_error(r.error());
  }
  return r;
}

void TraceRecorder::record(TraceRecord::Dir dir, const Bytes& data) {
  const auto at = std::chrono::duration_cast<std::chrono::microseconds>(clock_->now() - start_);
  *sink_ << format_trace_record(TraceRecord{at, dir, data, {}}) << '\n';
  sink_->flush();
}

void TraceRecorder::record_error(const Error& error) {
  const auto at = std::chrono::duration_cast<std::chrono::microseconds>(clock_->now() - start_);
  *sink_ << format_trace_record(TraceRecord{at, TraceRecord::Dir::Err, {}, std::string(to_string(error.kind)) + " " + error.what})
         << '\n';
  sink_->flush();
}

}  // namespace pychron
