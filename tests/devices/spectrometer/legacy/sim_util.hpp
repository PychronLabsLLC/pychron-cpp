#pragma once

// Shared helpers for legacy spectrometer driver tests.

#include <gtest/gtest.h>

#include <memory>
#include <string_view>

#include <toml++/toml.hpp>

#include "pychron/transport/sim_transport.hpp"

namespace legacy_test {

using namespace std::chrono_literals;

inline pychron::TransportOptions bus_options(std::string name = "legacy_bus") {
  pychron::TransportOptions o;
  o.name = std::move(name);
  o.timeout = 50ms;
  return o;
}

inline std::unique_ptr<pychron::SimTransport> opened(std::unique_ptr<pychron::SimTransport> sim) {
  EXPECT_TRUE(sim->open());
  return sim;
}

inline std::unique_ptr<pychron::SimTransport> open_scripted(std::vector<pychron::SimStep> steps) {
  return opened(pychron::SimTransport::scripted(std::move(steps), bus_options()));
}

inline std::unique_ptr<pychron::SimTransport> open_hooked(pychron::SimTransport::Hook hook) {
  return opened(pychron::SimTransport::hooked(std::move(hook), bus_options()));
}

inline pychron::SimStep step(std::string_view tx, std::string_view rx) {
  return pychron::SimStep{pychron::to_bytes(tx), pychron::to_bytes(rx), {}};
}

inline pychron::SimStep write_only(std::string_view tx) { return pychron::SimStep{pychron::to_bytes(tx), {}, {}}; }

inline pychron::SimStep raw(pychron::Bytes tx, pychron::Bytes rx) { return pychron::SimStep{std::move(tx), std::move(rx), {}}; }

inline toml::table table_of(std::string_view text) {
  auto r = toml::parse(text, std::string_view("spectrometer.toml"));
  EXPECT_TRUE(r) << r.error().description();
  return r ? std::move(r).table() : toml::table{};
}

inline void expect_verified(const pychron::SimTransport& sim) {
  auto v = sim.verify();
  EXPECT_TRUE(v) << (v ? "" : pychron::to_string(v.error()));
}

}  // namespace legacy_test
