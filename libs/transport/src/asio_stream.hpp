#pragma once

// Deadline-bounded I/O on an asio stream (serial_port or tcp::socket).
// Private to libs/transport: asio never appears in a public header.
//
// Every function runs on the transport's worker thread with an io_context
// that only that thread drives, so each op is started, run for at most its
// timeout, and cancelled (and its handler drained) before returning.

#include <array>
#include <asio.hpp>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/transport/bytes.hpp"
#include "pychron/transport/read_spec.hpp"

namespace pychron::detail {

Error io_error(std::string_view what, const asio::error_code& ec);
std::string millis(Duration d);

// Runs `io` until the started op completes or `timeout` elapses; on timeout
// calls `cancel` and drains the aborted handler. Returns true if it timed out.
template <class Cancel>
bool run_with_timeout(asio::io_context& io, const bool& done, Duration timeout, Cancel&& cancel) {
  io.restart();
  io.run_for(timeout);
  if (done) return false;
  cancel();
  io.restart();
  io.run();
  return true;
}

template <class Stream>
Result<void> write_all(asio::io_context& io, Stream& stream, const Bytes& tx, Duration timeout) {
  bool done = false;
  asio::error_code ec;
  asio::async_write(stream, asio::buffer(tx), [&](const asio::error_code& e, std::size_t) {
    ec = e;
    done = true;
  });
  const bool timed_out = run_with_timeout(io, done, timeout, [&] {
    asio::error_code ignored;
    stream.cancel(ignored);
  });
  if (timed_out && (!ec || ec == asio::error::operation_aborted))
    return fail(ErrorKind::Timeout, "write not complete within " + millis(timeout));
  if (ec) return fail(io_error("write failed", ec));
  return {};
}

// Reads until `pending` holds a complete frame per `spec`, returns it and
// leaves any following bytes in `pending` for the next read. `ChunkSize` is
// the most one receive takes; a datagram stream needs room for a whole
// datagram, since what does not fit is lost.
template <std::size_t ChunkSize = 512, class Stream>
Result<Bytes> read_frame(asio::io_context& io, Stream& stream, Bytes& pending, const ReadSpec& spec,
                         Duration timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    if (auto n = frame_length(spec, pending)) {
      const auto end = pending.begin() + static_cast<std::ptrdiff_t>(*n);
      Bytes frame(pending.begin(), end);
      pending.erase(pending.begin(), end);
      return frame;
    }
    const auto remaining = deadline - std::chrono::steady_clock::now();
    if (remaining <= Duration::zero()) {
      return fail(ErrorKind::Timeout, std::string(pending.empty() ? "no reply" : "incomplete reply") +
                                          " within " + millis(timeout));
    }

    std::vector<std::uint8_t> buf(ChunkSize);
    bool done = false;
    asio::error_code ec;
    std::size_t got = 0;
    stream.async_read_some(asio::buffer(buf), [&](const asio::error_code& e, std::size_t n) {
      ec = e;
      got = n;
      done = true;
    });
    run_with_timeout(io, done, remaining, [&] {
      asio::error_code ignored;
      stream.cancel(ignored);
    });
    pending.insert(pending.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(got));
    if (ec == asio::error::eof) {
      // The close is the end of an UntilClose frame (empty if nothing came).
      if (spec.kind == ReadSpec::Kind::UntilClose) return std::exchange(pending, Bytes{});
      return fail(ErrorKind::Io, "connection closed by peer");
    }
    if (ec && ec != asio::error::operation_aborted) return fail(io_error("read failed", ec));
  }
}

}  // namespace pychron::detail
