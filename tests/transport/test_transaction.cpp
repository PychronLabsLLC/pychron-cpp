// Transport::transaction: a multi-exchange sequence that no other caller's
// traffic can interleave with. Replaces driver-level mutexes around
// "select, then act" and "ask, then enquire" protocols.

#include <gtest/gtest.h>

#include <chrono>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/transport/sim_transport.hpp"
#include "pychron/transport/trace.hpp"
#include "pychron/transport/trace_recorder.hpp"

using namespace pychron;
using namespace std::chrono_literals;

namespace {

// Echo wire that records every tx in arrival order.
struct Wire {
  std::mutex mutex;
  std::vector<std::string> order;

  SimTransport::Hook hook() {
    return [this](const Bytes& tx) {
      std::string s(tx.begin(), tx.end());
      {
        std::lock_guard lock(mutex);
        order.push_back(s);
      }
      std::this_thread::sleep_for(200us);  // widen the window for interleaving
      Bytes reply(s.begin(), s.end());
      reply.push_back('\n');
      return reply;
    };
  }
};

Bytes bytes(const std::string& s) { return Bytes(s.begin(), s.end()); }

ReadSpec line() { return ReadSpec::until(std::string_view("\n")); }

// Every "a<i>" must be immediately followed on the wire by "b<i>".
void expect_pairs_adjacent(const std::vector<std::string>& order) {
  ASSERT_EQ(order.size() % 2, 0u);
  for (std::size_t i = 0; i < order.size(); i += 2) {
    ASSERT_EQ(order[i][0], 'a') << "at " << i;
    EXPECT_EQ(order[i + 1], "b" + order[i].substr(1)) << "pair split at " << i;
  }
}

TEST(TransportTransaction, ConcurrentSequencesNeverInterleave) {
  Wire wire;
  auto t = SimTransport::hooked(wire.hook());
  ASSERT_TRUE(t->open());

  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&, i] {
      for (int k = 0; k < 10; ++k) {
        const auto id = std::to_string(i) + "." + std::to_string(k);
        auto r = t->transaction([&]() -> Result<void> {
          if (auto a = t->exchange(bytes("a" + id), line()); !a) return fail(a.error());
          if (auto b = t->exchange(bytes("b" + id), line()); !b) return fail(b.error());
          return {};
        });
        EXPECT_TRUE(r) << r.error().what;
      }
    });
  }
  for (auto& th : threads) th.join();
  expect_pairs_adjacent(wire.order);
  EXPECT_EQ(wire.order.size(), 160u);
}

TEST(TransportTransaction, TransactReturnsBodyValueAndError) {
  Wire wire;
  auto t = SimTransport::hooked(wire.hook());
  ASSERT_TRUE(t->open());

  Result<int> ok = transact(*t, [&]() -> Result<int> {
    auto r = t->exchange(bytes("a1"), line());
    if (!r) return fail(r.error());
    return static_cast<int>(r->size());
  });
  ASSERT_TRUE(ok) << ok.error().what;
  EXPECT_EQ(*ok, 3);

  Result<int> bad = transact(*t, []() -> Result<int> { return fail(ErrorKind::Protocol, "nope", "dev"); });
  ASSERT_FALSE(bad);
  EXPECT_EQ(bad.error().kind, ErrorKind::Protocol);
  EXPECT_EQ(bad.error().what, "nope");
  EXPECT_EQ(bad.error().device, "dev");  // the body's error is returned unchanged
}

TEST(TransportTransaction, NestedTransactionRunsInline) {
  Wire wire;
  auto t = SimTransport::hooked(wire.hook());
  ASSERT_TRUE(t->open());
  auto r = t->transaction([&]() -> Result<void> {
    return t->transaction([&]() -> Result<void> {
      auto a = t->exchange(bytes("a0"), line());
      if (!a) return fail(a.error());
      auto b = t->exchange(bytes("b0"), line());
      if (!b) return fail(b.error());
      return {};
    });
  });
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_EQ(wire.order, (std::vector<std::string>{"a0", "b0"}));
}

TEST(TransportTransaction, ExchangesInsideAClosedTransportAreNotConnected) {
  Wire wire;
  auto t = SimTransport::hooked(wire.hook());
  auto r = transact(*t, [&]() -> Result<Bytes> { return t->exchange(bytes("a0"), line()); });
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::NotConnected);
  EXPECT_TRUE(wire.order.empty());
}

TEST(TransportTransaction, TraceRecorderForwardsAndKeepsWireOrder) {
  Wire wire;
  auto sink = std::make_shared<std::ostringstream>();
  SteadyClock clock;
  TraceRecorder rec(SimTransport::hooked(wire.hook()), sink, clock);
  ASSERT_TRUE(rec.open());

  std::vector<std::thread> threads;
  for (int i = 0; i < 6; ++i) {
    threads.emplace_back([&, i] {
      for (int k = 0; k < 5; ++k) {
        const auto id = std::to_string(i) + "." + std::to_string(k);
        auto r = rec.transaction([&]() -> Result<void> {
          if (auto a = rec.exchange(bytes("a" + id), line()); !a) return fail(a.error());
          if (auto b = rec.exchange(bytes("b" + id), line()); !b) return fail(b.error());
          return {};
        });
        EXPECT_TRUE(r) << r.error().what;
      }
    });
    // Plain exchanges from another thread must neither deadlock nor split a pair.
  }
  std::thread plain([&] {
    for (int k = 0; k < 20; ++k) (void)rec.exchange(bytes("p" + std::to_string(k)), line());
  });
  for (auto& th : threads) th.join();
  plain.join();

  // Pairs stay adjacent on the wire even with interleaved plain traffic.
  std::vector<std::string> pairs;
  for (const auto& s : wire.order) {
    if (s[0] != 'p') pairs.push_back(s);
  }
  expect_pairs_adjacent(pairs);
  for (std::size_t i = 0; i + 1 < wire.order.size(); ++i) {
    if (wire.order[i][0] == 'a') {
      EXPECT_EQ(wire.order[i + 1][0], 'b') << "plain traffic split a pair at " << i;
    }
  }

  // Trace file order is wire order.
  std::istringstream in(sink->str());
  auto records = parse_trace(in);
  ASSERT_TRUE(records) << records.error().what;
  std::vector<std::string> traced;
  for (const auto& r : *records) {
    if (r.dir == TraceRecord::Dir::Tx) traced.emplace_back(r.data.begin(), r.data.end());
  }
  EXPECT_EQ(traced, wire.order);
}

}  // namespace
