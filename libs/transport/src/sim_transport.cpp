#include "pychron/transport/sim_transport.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace pychron {

namespace {

struct Chunk {
  Bytes data;
  Duration delay{};
  bool garble = false;
};

// Corrupt one payload byte while keeping the frame's length and delimiters,
// so the reply still frames but fails the codec (bad value or checksum).
void garble(const ReadSpec& spec, Bytes& data) {
  if (data.empty()) return;
  switch (spec.kind) {
    case ReadSpec::Kind::ModbusRtu:
    case ReadSpec::Kind::ModbusTcp:
      data.back() = static_cast<std::uint8_t>(data.back() ^ 0xFF);  // CRC (RTU) / last payload byte (TCP)
      return;
    case ReadSpec::Kind::Terminator: {
      const auto end = frame_length(spec, data);
      if (end && *end <= spec.terminator.size()) return;  // empty payload: nothing to corrupt
      break;
    }
    case ReadSpec::Kind::AnyOf: {
      // Corrupt the first payload byte; leading set bytes are framing.
      for (auto& b : data) {
        if (std::find(spec.terminator.begin(), spec.terminator.end(), b) != spec.terminator.end()) continue;
        static constexpr std::uint8_t kAnyCandidates[] = {'?', '#', '~'};
        for (const std::uint8_t candidate : kAnyCandidates) {
          const bool in_set =
              std::find(spec.terminator.begin(), spec.terminator.end(), candidate) != spec.terminator.end();
          if (candidate != b && !in_set) {
            b = candidate;
            return;
          }
        }
        return;
      }
      return;  // empty payload: nothing to corrupt
    }
    case ReadSpec::Kind::FixedLength:
    case ReadSpec::Kind::UntilClose:
      break;
  }
  static constexpr std::uint8_t kCandidates[] = {'?', '#', '~'};
  for (const std::uint8_t candidate : kCandidates) {
    const bool in_terminator =
        std::find(spec.terminator.begin(), spec.terminator.end(), candidate) != spec.terminator.end();
    if (candidate != data[0] && !in_terminator) {
      data[0] = candidate;
      return;
    }
  }
}

std::string millis(Duration d) {
  return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(d).count()) + " ms";
}

}  // namespace

struct SimTransport::State {
  enum class Mode { Scripted, Hook };

  Mode mode = Mode::Scripted;
  Hook hook;  // set at construction, then read-only
  Unsolicited unsolicited;  // likewise; empty: none

  SteadyClock steady;
  const Clock* clock = &steady;  // the transport's; set at construction, then read-only

  std::mutex mutex;
  std::condition_variable cv;  // a read waiting for the unsolicited source; through the clock
  std::deque<SimStep> steps;
  std::deque<Chunk> rx;
  std::vector<Bytes> written;
  std::vector<std::string> unexpected;
  std::size_t drop = 0;
  Duration extra_delay{};
  bool garble_next = false;
  bool fail_open = false;
};

SimTransport::SimTransport(TransportOptions options, std::shared_ptr<State> state)
    : QueuedTransport(std::move(options)), state_(std::move(state)) {
  // Before the first job can run: nothing is queued until this returns.
  if (this->options().clock) state_->clock = this->options().clock;
}

SimTransport::~SimTransport() { shutdown(); }

std::unique_ptr<SimTransport> SimTransport::scripted(std::vector<SimStep> steps, TransportOptions options) {
  auto state = std::make_shared<State>();
  state->steps.assign(std::make_move_iterator(steps.begin()), std::make_move_iterator(steps.end()));
  return std::unique_ptr<SimTransport>(new SimTransport(std::move(options), std::move(state)));
}

std::unique_ptr<SimTransport> SimTransport::hooked(Hook hook, TransportOptions options, Unsolicited unsolicited) {
  auto state = std::make_shared<State>();
  state->mode = State::Mode::Hook;
  state->hook = std::move(hook);
  state->unsolicited = std::move(unsolicited);
  return std::unique_ptr<SimTransport>(new SimTransport(std::move(options), std::move(state)));
}

std::unique_ptr<SimTransport> SimTransport::replay(const std::vector<TraceRecord>& records,
                                                   TransportOptions options) {
  auto state = std::make_shared<State>();
  for (const auto& rec : records) {
    switch (rec.dir) {
      case TraceRecord::Dir::Tx:
        state->steps.push_back(SimStep{rec.data, {}, {}});
        break;
      case TraceRecord::Dir::Rx:
        if (state->steps.empty()) {
          state->rx.push_back(Chunk{rec.data, {}, false});  // unsolicited input before the first tx
        } else {
          auto& reply = state->steps.back().reply;
          reply.insert(reply.end(), rec.data.begin(), rec.data.end());
        }
        break;
      case TraceRecord::Dir::Err:
        break;  // an error after a tx replays as "no reply"
    }
  }
  return std::unique_ptr<SimTransport>(new SimTransport(std::move(options), std::move(state)));
}

Result<std::unique_ptr<SimTransport>> SimTransport::replay(const std::string& trace_path,
                                                           TransportOptions options) {
  auto records = load_trace(trace_path);
  if (!records) return fail(records.error());
  return replay(*records, std::move(options));
}

void SimTransport::drop_next(std::size_t n) {
  std::lock_guard lock(state_->mutex);
  state_->drop += n;
}

void SimTransport::delay_next(Duration delay) {
  std::lock_guard lock(state_->mutex);
  state_->extra_delay += delay;
}

void SimTransport::garble_next() {
  std::lock_guard lock(state_->mutex);
  state_->garble_next = true;
}

void SimTransport::fail_open_next() {
  std::lock_guard lock(state_->mutex);
  state_->fail_open = true;
}

void SimTransport::expect(SimStep step) {
  std::lock_guard lock(state_->mutex);
  state_->steps.push_back(std::move(step));
}

std::vector<Bytes> SimTransport::written() const {
  std::lock_guard lock(state_->mutex);
  return state_->written;
}

Result<void> SimTransport::verify() const {
  std::lock_guard lock(state_->mutex);
  std::string problems;
  for (const auto& u : state_->unexpected) problems += (problems.empty() ? "" : "; ") + u;
  if (!state_->steps.empty()) {
    problems += (problems.empty() ? "" : "; ") + std::to_string(state_->steps.size()) +
                " step(s) not consumed, next expects '" + escape(state_->steps.front().expect) + "'";
  }
  if (problems.empty()) return {};
  return fail(ErrorKind::Protocol, problems, name());
}

Result<void> SimTransport::do_open() {
  std::lock_guard lock(state_->mutex);
  if (state_->fail_open) {
    state_->fail_open = false;
    return fail(ErrorKind::Io, "simulated open failure");
  }
  return {};
}

void SimTransport::do_close() {
  std::lock_guard lock(state_->mutex);
  state_->rx.clear();
}

void SimTransport::do_discard_input() {
  std::lock_guard lock(state_->mutex);
  state_->rx.clear();
}

Result<void> SimTransport::do_write(const Bytes& tx, Duration) {
  auto& s = *state_;
  Bytes reply;
  Duration delay{};
  if (s.mode == State::Mode::Hook) {
    // Input already due arrives before this command's reply, as on a wire.
    if (s.unsolicited) {
      std::lock_guard lock(s.mutex);
      if (Bytes due = s.unsolicited(); !due.empty()) s.rx.push_back(Chunk{std::move(due), {}, false});
    }
    reply = s.hook ? s.hook(tx) : Bytes{};  // outside the lock: hooks may be slow
  }

  std::lock_guard lock(s.mutex);
  s.written.push_back(tx);
  if (s.mode == State::Mode::Scripted) {
    if (s.steps.empty() || s.steps.front().expect != tx) {
      std::string what = "unexpected tx '" + escape(tx) + "'";
      what += s.steps.empty() ? " (script exhausted)" : " (expected '" + escape(s.steps.front().expect) + "')";
      s.unexpected.push_back(what);
      return fail(ErrorKind::Protocol, what);
    }
    reply = std::move(s.steps.front().reply);
    delay = s.steps.front().delay;
    s.steps.pop_front();
  }

  if (reply.empty()) return {};
  if (s.drop > 0) {
    --s.drop;
    return {};
  }
  s.rx.push_back(Chunk{std::move(reply), delay + s.extra_delay, s.garble_next});
  s.extra_delay = Duration{};
  s.garble_next = false;
  return {};
}

Result<Bytes> SimTransport::do_read(const ReadSpec& rs, Duration timeout) {
  // With an unsolicited source the read waits, in the transport's clock and
  // like a socket, for input the source produces; without one it does not
  // wait at all: a reply's delay is compared with the timeout.
  const Clock& clock = *state_->clock;
  const TimePoint deadline = clock.now() + timeout;
  std::unique_lock lock(state_->mutex);
  auto& rx = state_->rx;
  Bytes buf;
  std::optional<std::size_t> n;
  for (;;) {
    if (state_->unsolicited) {
      if (Bytes more = state_->unsolicited(); !more.empty()) rx.push_back(Chunk{std::move(more), {}, false});
    }
    buf.clear();
    for (auto& chunk : rx) {
      if (chunk.delay >= timeout) break;  // arrives too late for this read
      if (chunk.garble) {
        garble(rs, chunk.data);
        chunk.garble = false;
      }
      buf.insert(buf.end(), chunk.data.begin(), chunk.data.end());
      if ((n = frame_length(rs, buf))) break;
    }
    // A simulated peer has said all it will once its reply is queued: for
    // UntilClose that is the frame, as if it then closed.
    if (!n && rs.kind == ReadSpec::Kind::UntilClose && !buf.empty()) n = buf.size();
    if (n || !state_->unsolicited) break;
    const TimePoint now = clock.now();
    if (now >= deadline) break;
    // The source is polled: nothing notifies, the wait ends at its deadline.
    clock.wait_until(state_->cv, lock, std::min(deadline, now + std::chrono::milliseconds(1)));
  }
  if (!n) {
    return fail(ErrorKind::Timeout, (buf.empty() ? "no reply within " : "incomplete reply within ") +
                                        millis(timeout));
  }

  // Consume exactly the framed bytes; any remainder stays buffered.
  std::size_t left = *n;
  while (left > 0) {
    auto& front = rx.front();
    if (front.data.size() <= left) {
      left -= front.data.size();
      rx.pop_front();
    } else {
      front.data.erase(front.data.begin(), front.data.begin() + static_cast<std::ptrdiff_t>(left));
      front.delay = Duration{};
      left = 0;
    }
  }
  buf.resize(*n);
  return buf;
}

}  // namespace pychron
