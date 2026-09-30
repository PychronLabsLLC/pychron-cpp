#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/transport/bytes.hpp"
#include "pychron/transport/health.hpp"
#include "pychron/transport/read_spec.hpp"

namespace pychron {

class SignalBus;

// One physical channel (serial port, socket, simulated wire). Knows how bytes
// reach the wire; never what they mean or which device sent them.
//
// All calls may be made from any thread. Implementations serialize them so
// that exchange() is atomic on the bus: no other traffic interleaves between
// its write and its read.
class Transport {
 public:
  // Pass as `timeout` to use the transport's configured timeout.
  static constexpr Duration kDefaultTimeout = Duration::zero();

  virtual ~Transport() = default;

  virtual const std::string& name() const = 0;

  virtual Result<void> open() = 0;
  virtual void close() = 0;

  // Discard stale input, write `tx`, read one reply framed by `rs`.
  virtual Result<Bytes> exchange(Bytes tx, ReadSpec rs, Duration timeout = kDefaultTimeout) = 0;
  virtual Result<void> write(Bytes tx) = 0;
  virtual Result<Bytes> read(ReadSpec rs, Duration timeout = kDefaultTimeout) = 0;

  // Runs `body` with exclusive use of the bus: exchange()/write()/read() calls
  // it makes on this transport run back to back, and no other caller's
  // traffic interleaves until it returns. For multi-step protocols ("select
  // bank, then switch relay"; "send mnemonic, then ENQ") so drivers need no
  // locks of their own. Each call inside keeps its own retry and health
  // handling; the sequence as a whole is not retried. Nested transactions
  // run inline. Returns body's result, or Cancelled if the transport shut
  // down before body could run. Prefer the typed transact() helper.
  virtual Result<void> transaction(std::function<Result<void>()> body) = 0;

  virtual Health health() const = 0;
};

// Typed wrapper over Transport::transaction: returns whatever `body`
// returns (a Result<T>), unchanged, or the transport's error if body never ran.
template <class F>
auto transact(Transport& transport, F&& body) -> std::invoke_result_t<F&> {
  using R = std::invoke_result_t<F&>;
  std::optional<R> out;
  auto r = transport.transaction([&]() -> Result<void> {
    out.emplace(body());
    if (!*out) return fail(out->error());
    return {};
  });
  if (out) return std::move(*out);
  return R(fail(std::move(r).error()));
}

struct TransportOptions {
  std::string name;
  // Per-attempt reply timeout used when a call passes kDefaultTimeout.
  Duration timeout = std::chrono::milliseconds(500);
  // Extra attempts for exchange()/write() after a Timeout or Io failure.
  int retries = 0;
  // Consecutive failed calls after which health() reports Down.
  std::uint64_t down_after = 3;
  // Time source for Health::last_ok; a SteadyClock is used when null.
  const Clock* clock = nullptr;
  // When set, a TransportHealth event is published on every state change.
  SignalBus* bus = nullptr;
};

// Base for concrete channels. Owns one worker thread and a FIFO job queue:
// every public call is enqueued and the caller blocks until the worker has
// run it. Retries, default timeouts, error attribution and health tracking
// are applied here, once; subclasses implement only the raw primitives,
// which always run on the worker thread.
//
// Subclasses must call shutdown() from their destructor so the worker stops
// before their primitives are destroyed.
class QueuedTransport : public Transport {
 public:
  explicit QueuedTransport(TransportOptions options);
  ~QueuedTransport() override;

  QueuedTransport(const QueuedTransport&) = delete;
  QueuedTransport& operator=(const QueuedTransport&) = delete;

  const std::string& name() const final;
  Result<void> open() final;
  void close() final;
  Result<Bytes> exchange(Bytes tx, ReadSpec rs, Duration timeout = kDefaultTimeout) final;
  Result<void> write(Bytes tx) final;
  Result<Bytes> read(ReadSpec rs, Duration timeout = kDefaultTimeout) final;
  // One queued job that runs `body` on the worker; calls body makes run inline.
  Result<void> transaction(std::function<Result<void>()> body) final;
  Health health() const final;

  const TransportOptions& options() const noexcept;

 protected:
  // Stops the worker, fails queued calls with Cancelled and closes the
  // channel. Idempotent.
  void shutdown();

  virtual Result<void> do_open() = 0;
  virtual void do_close() = 0;
  // Drop any unread input before an exchange. Default: nothing buffered.
  virtual void do_discard_input() {}
  virtual Result<void> do_write(const Bytes& tx, Duration timeout) = 0;
  virtual Result<Bytes> do_read(const ReadSpec& rs, Duration timeout) = 0;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace pychron
