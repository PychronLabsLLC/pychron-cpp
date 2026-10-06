#pragma once

// What every IValveActuator must do, whatever the hardware (plan 2026-10-05,
// task A0). A driver's test builds a ValveRig around the driver and its
// simulated wire and calls expect_valve_conformance().
//
//   - open, then read: Open; close, then read: Closed; other addresses
//     untouched.
//   - an address the driver cannot use is a Config error and nothing is
//     sent.
//   - a device that does not answer is an error (Timeout, or Io from a
//     driver that reconnects), never a state.
//   - a reply the codec rejects is a Protocol error, never a state.
//   - after either, the next command works: no stale bytes are taken for its
//     reply.

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <optional>
#include <string>

#include "pychron/devices/capabilities.hpp"
#include "pychron/transport/sim_transport.hpp"

namespace pychron::test {

// Wraps a SimTransport hook: counts what is sent and can stop answering or
// answer garbage, for rigs built on SimTransport::hooked().
class WireTap {
 public:
  enum class Mode { Normal, Silent, Garbage };

  explicit WireTap(SimTransport::Hook inner, Bytes garbage) : inner_(std::move(inner)), garbage_(std::move(garbage)) {}

  SimTransport::Hook hook() {
    return [this](const Bytes& tx) -> Bytes {
      ++sent_;
      switch (mode_.load()) {
        case Mode::Silent:
          return {};
        case Mode::Garbage:
          return garbage_;
        case Mode::Normal:
          break;
      }
      return inner_(tx);
    };
  }

  void set(Mode mode) { mode_ = mode; }
  std::size_t sent() const { return sent_; }

 private:
  SimTransport::Hook inner_;
  Bytes garbage_;
  std::atomic<Mode> mode_{Mode::Normal};
  std::atomic<std::size_t> sent_{0};
};

struct ValveRig {
  virtual ~ValveRig() = default;
  virtual IValveActuator& actuator() = 0;
  // Two distinct addresses the driver accepts.
  virtual ValveAddress first() = 0;
  virtual ValveAddress second() = 0;
  // An address the driver refuses, or nullopt if it accepts any.
  virtual std::optional<ValveAddress> bad() { return std::nullopt; }
  // The wire, or nullptr for an actuator that has none (sim_valves).
  virtual WireTap* tap() { return nullptr; }
};

inline ValveState state_of(const Result<ValveState>& r) { return r ? *r : ValveState::Unknown; }

inline void expect_valve_conformance(ValveRig& rig) {
  auto& a = rig.actuator();
  const auto first = rig.first();
  const auto second = rig.second();

  SCOPED_TRACE("valve conformance");
  ASSERT_TRUE(a.close(first));
  ASSERT_TRUE(a.close(second));
  ASSERT_TRUE(a.open(first)) << "open";
  EXPECT_EQ(state_of(a.read(first)), ValveState::Open);
  EXPECT_EQ(state_of(a.read(second)), ValveState::Closed) << "an open touched another address";
  ASSERT_TRUE(a.close(first)) << "close";
  EXPECT_EQ(state_of(a.read(first)), ValveState::Closed);

  if (auto bad = rig.bad()) {
    const std::size_t before = rig.tap() ? rig.tap()->sent() : 0;
    for (auto r : {a.open(*bad), a.close(*bad)}) {
      ASSERT_FALSE(r);
      EXPECT_EQ(r.error().kind, ErrorKind::Config) << r.error().what;
    }
    auto read = a.read(*bad);
    ASSERT_FALSE(read);
    EXPECT_EQ(read.error().kind, ErrorKind::Config) << read.error().what;
    if (rig.tap()) {
      EXPECT_EQ(rig.tap()->sent(), before) << "a refused address reached the wire";
    }
  }

  if (auto* tap = rig.tap()) {
    tap->set(WireTap::Mode::Silent);
    for (auto r : {a.open(first), a.close(first)}) {
      ASSERT_FALSE(r) << "no answer passed as success";
      EXPECT_TRUE(r.error().kind == ErrorKind::Timeout || r.error().kind == ErrorKind::Io) << r.error().what;
    }
    auto silent_read = a.read(first);
    ASSERT_FALSE(silent_read) << "no answer read as " << pychron::to_string(*silent_read);

    tap->set(WireTap::Mode::Garbage);
    auto garbled = a.read(first);
    ASSERT_FALSE(garbled) << "garbage read as " << pychron::to_string(*garbled);
    EXPECT_EQ(garbled.error().kind, ErrorKind::Protocol) << garbled.error().what;

    tap->set(WireTap::Mode::Normal);
    ASSERT_TRUE(a.open(second)) << "after errors";
    EXPECT_EQ(state_of(a.read(second)), ValveState::Open);
    ASSERT_TRUE(a.close(second));
    EXPECT_EQ(state_of(a.read(second)), ValveState::Closed);
  }
}

}  // namespace pychron::test
