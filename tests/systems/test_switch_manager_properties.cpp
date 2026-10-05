// Property-style interlock tests: random networks and random open/close
// sequences (seeded, reproducible) never violate declared interlocks.

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "pychron/systems/switch_manager.hpp"

using namespace pychron;
using namespace pychron::systems;

namespace {

// Ideal hardware: every command lands, reads report the truth.
class IdealActuator final : public IValveActuator {
 public:
  Result<void> open(const ValveAddress& a) override { return set(a, ValveState::Open); }
  Result<void> close(const ValveAddress& a) override { return set(a, ValveState::Closed); }
  Result<ValveState> read(const ValveAddress& a) override {
    std::lock_guard lk(m_);
    auto it = hw_.find(a.value);
    return it == hw_.end() ? ValveState::Closed : it->second;
  }

 private:
  Result<void> set(const ValveAddress& a, ValveState s) {
    std::lock_guard lk(m_);
    hw_[a.value] = s;
    return {};
  }
  std::mutex m_;
  std::map<std::string, ValveState> hw_;
};

struct Network {
  std::vector<SwitchSpec> specs;
};

// n valves V0..Vn-1. Negative interlocks between random pairs; positive
// interlocks only point to lower indices so they are acyclic, and never to a
// valve already listed as a negative interlock.
Network random_network(std::mt19937& rng, int n) {
  Network net;
  std::bernoulli_distribution neg(0.2), pos(0.15);
  for (int i = 0; i < n; ++i) {
    SwitchSpec s;
    s.name = "V" + std::to_string(i);
    s.actuator = "act";
    s.address = ValveAddress{std::to_string(i)};
    for (int j = 0; j < n; ++j) {
      if (j == i) continue;
      const auto other = "V" + std::to_string(j);
      if (neg(rng)) {
        s.interlocks.push_back(other);
      } else if (j < i && pos(rng)) {
        s.positive_interlocks.push_back(other);
      }
    }
    net.specs.push_back(std::move(s));
  }
  return net;
}

bool is_open(const std::map<std::string, ValveState>& s, const std::string& v) { return s.at(v) == ValveState::Open; }

}  // namespace

TEST(SwitchManagerProperty, RandomSequencesNeverViolateInterlocks) {
  constexpr int kNetworks = 40;
  constexpr int kSteps = 300;
  std::size_t accepted_opens = 0, refused = 0;

  for (int seed = 0; seed < kNetworks; ++seed) {
    std::mt19937 rng(static_cast<std::mt19937::result_type>(seed));
    std::uniform_int_distribution<int> size_dist(2, 10);
    const int n = size_dist(rng);
    auto net = random_network(rng, n);
    IdealActuator act;
    auto made = SwitchManager::create(net.specs, [&](const std::string&) -> IValveActuator* { return &act; });
    ASSERT_TRUE(made) << "seed " << seed << ": " << made.error().what;
    auto& mgr = **made;
    ASSERT_TRUE(mgr.refresh());

    std::uniform_int_distribution<int> pick(0, n - 1);
    std::bernoulli_distribution want_open(0.6);
    for (int step = 0; step < kSteps; ++step) {
      const auto& spec = net.specs[static_cast<std::size_t>(pick(rng))];
      const auto op = want_open(rng) ? SwitchOp::Open : SwitchOp::Close;
      const auto before = mgr.states();
      auto r = mgr.actuate(spec.name, op, "prop");
      const auto after = mgr.states();

      // Closing always succeeds on ideal hardware.
      if (op == SwitchOp::Close) {
        ASSERT_TRUE(r) << "seed " << seed << " step " << step;
        continue;
      }
      if (!r) {
        ++refused;
        EXPECT_EQ(r.error().kind, ErrorKind::Interlock);
        EXPECT_EQ(before, after) << "refused actuation changed state";
        continue;
      }
      ++accepted_opens;
      // Positive interlocks held at the moment of opening.
      for (const auto& p : spec.positive_interlocks) {
        EXPECT_TRUE(is_open(before, p)) << "seed " << seed << ": " << spec.name << " opened with " << p
                                        << " closed";
      }
      // Declared negative pairs are never open together.
      for (const auto& s : net.specs) {
        for (const auto& other : s.interlocks) {
          EXPECT_FALSE(is_open(after, s.name) && is_open(after, other))
              << "seed " << seed << " step " << step << ": " << s.name << " and " << other << " both open";
        }
      }
    }
  }
  // The generator exercised both outcomes.
  EXPECT_GT(accepted_opens, 100u);
  EXPECT_GT(refused, 100u);
}

// An open refused for interlock would have been accepted had its reason been
// absent: refusals are never spurious.
TEST(SwitchManagerProperty, RefusalsAreExplainedByDeclaredInterlocks) {
  for (int seed = 100; seed < 130; ++seed) {
    std::mt19937 rng(static_cast<std::mt19937::result_type>(seed));
    auto net = random_network(rng, 8);
    IdealActuator act;
    auto made = SwitchManager::create(net.specs, [&](const std::string&) -> IValveActuator* { return &act; });
    ASSERT_TRUE(made);
    auto& mgr = **made;
    ASSERT_TRUE(mgr.refresh());

    // Everything that points at a valve through a negative interlock.
    std::map<std::string, std::set<std::string>> exclusive;
    for (const auto& s : net.specs) {
      for (const auto& o : s.interlocks) {
        exclusive[s.name].insert(o);
        exclusive[o].insert(s.name);
      }
    }

    std::uniform_int_distribution<int> pick(0, 7);
    std::bernoulli_distribution want_open(0.6);
    for (int step = 0; step < 200; ++step) {
      const auto& spec = net.specs[static_cast<std::size_t>(pick(rng))];
      const auto op = want_open(rng) ? SwitchOp::Open : SwitchOp::Close;
      const auto before = mgr.states();
      auto r = mgr.actuate(spec.name, op, "prop");
      if (op == SwitchOp::Close || r) continue;
      bool explained = false;
      for (const auto& o : exclusive[spec.name]) explained |= is_open(before, o);
      for (const auto& p : spec.positive_interlocks) explained |= !is_open(before, p);
      EXPECT_TRUE(explained) << "seed " << seed << ": " << spec.name << " refused: " << r.error().what;
    }
  }
}

namespace {

// Valves whose channels may be wired backwards, read back either through
// their actuator or through a second device (plan 2026-10-05, task 0.2).
// Tracks what each valve physically is.
class WiredLine {
 public:
  struct Wiring {
    bool channel_inverted = false;  // open() closes the valve
    bool source = false;            // read through "src" at "s<address>"
    bool source_inverted = false;   // that input reads Open when closed
  };

  class Actuator final : public IValveActuator {
   public:
    explicit Actuator(WiredLine& line) : line_(line) {}
    Result<void> open(const ValveAddress& a) override { return line_.drive(a.value, true); }
    Result<void> close(const ValveAddress& a) override { return line_.drive(a.value, false); }
    Result<ValveState> read(const ValveAddress& a) override { return line_.channel(a.value); }

   private:
    WiredLine& line_;
  };

  class Source final : public IValveActuator {
   public:
    explicit Source(WiredLine& line) : line_(line) {}
    Result<void> open(const ValveAddress&) override { return fail(ErrorKind::Config, "a sensor"); }
    Result<void> close(const ValveAddress&) override { return fail(ErrorKind::Config, "a sensor"); }
    Result<ValveState> read(const ValveAddress& a) override { return line_.sensed(a.value.substr(1)); }

   private:
    WiredLine& line_;
  };

  std::map<std::string, Wiring> wiring;    // by address
  std::map<std::string, bool> physical;    // by address: the valve is open
  Actuator actuator{*this};
  Source source{*this};

 private:
  static ValveState of(bool open) { return open ? ValveState::Open : ValveState::Closed; }
  Result<void> drive(const std::string& a, bool channel_on) {
    physical[a] = channel_on != wiring[a].channel_inverted;
    return {};
  }
  ValveState channel(const std::string& a) { return of(physical[a] != wiring[a].channel_inverted); }
  ValveState sensed(const std::string& a) { return of(physical[a] != wiring[a].source_inverted); }
};

}  // namespace

TEST(SwitchManagerProperty, WiringNeverChangesWhatTheValvesDo) {
  for (int seed = 200; seed < 240; ++seed) {
    std::mt19937 rng(static_cast<std::mt19937::result_type>(seed));
    auto net = random_network(rng, 8);
    WiredLine line;
    std::bernoulli_distribution coin(0.5);
    for (auto& s : net.specs) {
      auto& w = line.wiring[s.address.value];
      w.channel_inverted = coin(rng);
      w.source = coin(rng);
      w.source_inverted = coin(rng);
      s.inverted = w.channel_inverted;
      if (w.source) s.state_source = StateSource{"src", ValveAddress{"s" + s.address.value}, w.source_inverted};
      line.physical[s.address.value] = false;
    }
    auto made = SwitchManager::create(net.specs, [&](const std::string& n) -> IValveActuator* {
      return n == "src" ? static_cast<IValveActuator*>(&line.source) : &line.actuator;
    });
    ASSERT_TRUE(made) << made.error().what;
    auto& mgr = **made;
    ASSERT_TRUE(mgr.refresh());

    std::uniform_int_distribution<int> pick(0, 7);
    std::bernoulli_distribution want_open(0.6);
    for (int step = 0; step < 200; ++step) {
      const auto& spec = net.specs[static_cast<std::size_t>(pick(rng))];
      const auto op = want_open(rng) ? SwitchOp::Open : SwitchOp::Close;
      auto r = mgr.actuate(spec.name, op, "prop");
      if (r) {
        EXPECT_EQ(line.physical[spec.address.value], op == SwitchOp::Open) << "seed " << seed << ": " << spec.name;
      }
      // What the manager records is what the valves are.
      for (const auto& s : net.specs) {
        EXPECT_EQ(mgr.state(s.name).value(),
                  line.physical[s.address.value] ? ValveState::Open : ValveState::Closed)
            << "seed " << seed << " step " << step << ": " << s.name;
      }
      // Declared negative pairs are never physically open together.
      for (const auto& s : net.specs) {
        for (const auto& other : s.interlocks) {
          const auto& o = *std::find_if(net.specs.begin(), net.specs.end(),
                                        [&](const SwitchSpec& x) { return x.name == other; });
          EXPECT_FALSE(line.physical[s.address.value] && line.physical[o.address.value])
              << "seed " << seed << " step " << step << ": " << s.name << " and " << other << " both open";
        }
      }
    }
  }
}
