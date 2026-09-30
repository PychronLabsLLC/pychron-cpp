#pragma once

#include <memory>
#include <mutex>
#include <ostream>
#include <string>

#include "pychron/core/clock.hpp"
#include "pychron/transport/trace.hpp"
#include "pychron/transport/transport.hpp"

namespace pychron {

// Decorator: forwards every call to `inner` and appends tx/rx/err records
// with timestamps to `sink`, in a format SimTransport::replay() reads.
//
// Calls are held under the recorder's lock for their whole duration so the
// file order is exactly the wire order. This costs nothing: the inner
// transport already serializes all traffic.
class TraceRecorder final : public Transport {
 public:
  TraceRecorder(std::unique_ptr<Transport> inner, std::shared_ptr<std::ostream> sink,
                const Clock& clock);

  // Opens (truncates) `path` for writing; Io error if it cannot.
  static Result<std::unique_ptr<TraceRecorder>> to_file(std::unique_ptr<Transport> inner,
                                                        const std::string& path, const Clock& clock);

  const std::string& name() const override;
  Result<void> open() override;
  void close() override;
  Result<Bytes> exchange(Bytes tx, ReadSpec rs, Duration timeout = kDefaultTimeout) override;
  Result<void> write(Bytes tx) override;
  Result<Bytes> read(ReadSpec rs, Duration timeout = kDefaultTimeout) override;
  Health health() const override;

  Transport& inner() noexcept { return *inner_; }

 private:
  void record(TraceRecord::Dir dir, const Bytes& data);
  void record_error(const Error& error);

  std::unique_ptr<Transport> inner_;
  std::shared_ptr<std::ostream> sink_;
  const Clock* clock_;
  TimePoint start_;
  std::mutex mutex_;
};

}  // namespace pychron
