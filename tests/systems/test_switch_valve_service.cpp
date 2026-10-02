// SwitchValveService: the script host's valve vocabulary routed through
// SwitchManager, so locks, ownership and interlocks apply to scripts.

#include "pychron/systems/switch_valve_service.hpp"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>

using namespace pychron;
using namespace pychron::systems;

namespace {

class MemoryActuator final : public IValveActuator {
 public:
  Result<void> open(const ValveAddress& a) override {
    hw[a.value] = ValveState::Open;
    return {};
  }
  Result<void> close(const ValveAddress& a) override {
    hw[a.value] = ValveState::Closed;
    return {};
  }
  Result<ValveState> read(const ValveAddress& a) override {
    auto it = hw.find(a.value);
    return it == hw.end() ? ValveState::Closed : it->second;
  }
  std::map<std::string, ValveState> hw;
};

class SwitchValveServiceTest : public ::testing::Test {
 protected:
  // Not a designated initializer: clang 18 warns about every field one omits.
  static SwitchSpec spec(std::string name, std::string address) {
    SwitchSpec s;
    s.name = std::move(name);
    s.actuator = "relay";
    s.address = {std::move(address)};
    return s;
  }

  void SetUp() override {
    SwitchSpec a = spec("A", "1");
    a.interlocks = {"B"};
    SwitchSpec b = spec("B", "2");
    SwitchSpec pump = spec("pump", "3");
    pump.kind = SwitchKind::Switch;
    auto m = SwitchManager::create({a, b, pump}, [this](const std::string&) { return &relay; });
    ASSERT_TRUE(m) << to_string(m.error());
    manager = std::move(*m);
    ASSERT_TRUE(manager->refresh());
    service = std::make_unique<SwitchValveService>(*manager, "script:run-1");
  }

  MemoryActuator relay;
  std::unique_ptr<SwitchManager> manager;
  std::unique_ptr<SwitchValveService> service;
};

TEST_F(SwitchValveServiceTest, OpenAndCloseActuateAndReadBack) {
  ASSERT_TRUE(service->open("A"));
  EXPECT_EQ(relay.hw["1"], ValveState::Open);
  auto open = service->is_open("A");
  ASSERT_TRUE(open);
  EXPECT_TRUE(*open);
  ASSERT_TRUE(service->close("A"));
  auto closed = service->is_closed("A");
  ASSERT_TRUE(closed);
  EXPECT_TRUE(*closed);
}

TEST_F(SwitchValveServiceTest, InterlocksApplyToScripts) {
  ASSERT_TRUE(service->open("B"));
  auto r = service->open("A");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Interlock);
  EXPECT_FALSE(relay.hw.contains("1")) << "refusal sends nothing";
}

TEST_F(SwitchValveServiceTest, OwnershipByAnotherActorRefuses) {
  ASSERT_TRUE(manager->claim("B", "operator"));
  auto r = service->open("B");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Interlock);
  ASSERT_TRUE(manager->claim("A", "script:run-1"));
  EXPECT_TRUE(service->open("A"));
}

TEST_F(SwitchValveServiceTest, LockUnlockAreSoftwareLocks) {
  ASSERT_TRUE(service->lock("A"));
  auto info = manager->info("A");
  ASSERT_TRUE(info);
  EXPECT_TRUE(info->locked);
  EXPECT_FALSE(service->open("A"));
  ASSERT_TRUE(service->unlock("A"));
  EXPECT_TRUE(service->open("A"));
}

TEST_F(SwitchValveServiceTest, UnknownNamesAreConfigErrors) {
  EXPECT_FALSE(service->contains("Z"));
  for (auto r : {service->open("Z"), service->close("Z"), service->lock("Z")}) {
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().kind, ErrorKind::Config);
  }
  auto s = service->is_open("Z");
  ASSERT_FALSE(s);
  EXPECT_EQ(s.error().kind, ErrorKind::Config);
}

TEST_F(SwitchValveServiceTest, NamesListEverySwitchForTheStaticCheck) {
  EXPECT_EQ(service->names(), (std::vector<std::string>{"A", "B", "pump"}));
  EXPECT_TRUE(service->contains("pump"));
  EXPECT_EQ(service->actor(), "script:run-1");
}

TEST_F(SwitchValveServiceTest, ReportsValvesCapabilityInServices) {
  extraction::ExtractionServices services{nullptr, service.get(), nullptr};
  EXPECT_EQ(extraction::capabilities(services),
            extraction::CapabilitySet{extraction::Capability::Valves});
}

}  // namespace
