#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "pychron/transport/trace.hpp"
#include "pychron/transport/transport.hpp"

namespace pychron {

// One scripted wire interaction: when `expect` is written, `reply` becomes
// readable after `delay`. An empty reply models a write-only command.
struct SimStep {
  Bytes expect;
  Bytes reply;
  Duration delay{};
};

// Fakes the wire. Same queue/worker/retry/health machinery as real
// transports (it is a QueuedTransport), so drivers run unmodified.
//
// Modes:
//   scripted  ordered SimSteps; any unexpected tx is recorded as a failure
//             (see verify()) and the write fails with Protocol.
//   replay    steps rebuilt from a TraceRecorder file.
//   hook      a callback maps each tx to a reply (empty = no reply).
//
// Time is virtual: a reply delayed by at least the read timeout is a Timeout
// and stays buffered as late input, discarded before the next exchange.
class SimTransport final : public QueuedTransport {
 public:
  using Hook = std::function<Bytes(const Bytes& tx)>;

  static std::unique_ptr<SimTransport> scripted(std::vector<SimStep> steps, TransportOptions options = {});
  static std::unique_ptr<SimTransport> hooked(Hook hook, TransportOptions options = {});
  // Config error if the trace cannot be read or parsed.
  static Result<std::unique_ptr<SimTransport>> replay(const std::string& trace_path,
                                                      TransportOptions options = {});
  static std::unique_ptr<SimTransport> replay(const std::vector<TraceRecord>& records,
                                              TransportOptions options = {});

  ~SimTransport() override;

  // Fault injection; each applies to the next reply(s) produced by a write.
  void drop_next(std::size_t n = 1);      // replies are never delivered
  void delay_next(Duration delay);        // next reply delayed (added to step delay)
  void garble_next();                     // next reply is corrupted in place
  void fail_open_next();                  // next open() fails with Io

  // Append steps to a scripted/replay transport.
  void expect(SimStep step);

  // Every tx written, in order (including unexpected ones).
  std::vector<Bytes> written() const;
  // Ok if every step was consumed and no unexpected tx occurred; otherwise
  // a Protocol error describing the first problem(s).
  Result<void> verify() const;

 private:
  struct State;
  explicit SimTransport(TransportOptions options, std::shared_ptr<State> state);

  Result<void> do_open() override;
  void do_close() override;
  void do_discard_input() override;
  Result<void> do_write(const Bytes& tx, Duration timeout) override;
  Result<Bytes> do_read(const ReadSpec& rs, Duration timeout) override;

  std::shared_ptr<State> state_;
};

}  // namespace pychron
