// One Qtegra connection shared by the spectrometer driver and borrowers on a
// kind = "link" transport (plan 2026-10-05, task 0.3).
#include "pychron/devices/spectrometer/qtegra_link.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "pychron/devices/capabilities.hpp"
#include "pychron/devices/driver_registry.hpp"
#include "pychron/devices/spectrometer/thermo_qtegra.hpp"
#include "pychron/devices/spectrometer/thermo_qtegra_sim.hpp"
#include "pychron/transport/link_transport.hpp"
#include "spectrometer/legacy/sim_util.hpp"

using namespace pychron;
using namespace pychron::spectrometer;
using namespace legacy_test;

namespace {

const SteadyClock kClock;

// Forwards to another transport; while `fail_exchanges` > 0 an exchange fails
// with Io before reaching the wire (a dropped connection).
class Flaky final : public Transport {
 public:
  explicit Flaky(Transport& inner) : inner_(inner) {}
  const std::string& name() const override { return inner_.name(); }
  Result<void> open() override {
    ++opens;
    return inner_.open();
  }
  void close() override { inner_.close(); }
  Result<Bytes> exchange(Bytes tx, ReadSpec rs, Duration timeout) override {
    if (fail_exchanges > 0) {
      --fail_exchanges;
      return fail(ErrorKind::Io, "connection reset", inner_.name());
    }
    return inner_.exchange(std::move(tx), std::move(rs), timeout);
  }
  Result<void> write(Bytes tx) override { return inner_.write(std::move(tx)); }
  Result<Bytes> read(ReadSpec rs, Duration timeout) override { return inner_.read(std::move(rs), timeout); }
  Result<void> transaction(std::function<Result<void>()> body) override { return inner_.transaction(std::move(body)); }
  Health health() const override { return inner_.health(); }

  std::atomic<int> fail_exchanges{0};
  std::atomic<int> opens{0};

 private:
  Transport& inner_;
};

struct Rig {
  std::shared_ptr<QtegraSimModel> model = std::make_shared<QtegraSimModel>();
  std::unique_ptr<SimTransport> sim = open_hooked(qtegra_sim_hook(model));
};

Result<std::unique_ptr<Device>> make_spectrometer(Transport& transport, std::string_view toml) {
  return DriverRegistry::global().create("thermo_qtegra", transport, table_of(toml), DriverContext{"argus"});
}

std::string text(const Result<Bytes>& reply) {
  EXPECT_TRUE(reply) << reply.error().what;
  if (!reply) return {};
  std::string s = to_string(*reply);
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
  return s;
}

}  // namespace

TEST(QtegraLink, ABorrowerSpeaksOverTheSpectrometersConnection) {
  Rig rig;
  rig.model->params["Ion Gauge MS Readback"] = 2.5e-9;
  auto owner = make_spectrometer(*rig.sim, "link = \"argus_link\"");
  ASSERT_TRUE(owner) << owner.error().what;

  LinkTransport borrowed("qtegra_valves", "argus_link");
  auto handle = make_qtegra_link(borrowed, "unused", codec::qtegra::kDefaultTerminator, kClock);
  ASSERT_TRUE(handle) << handle.error().what;
  EXPECT_FALSE(handle->owner());
  auto link = handle->get();
  ASSERT_TRUE(link) << link.error().what;
  EXPECT_EQ(text((*link)->ask("GetParameter Ion Gauge MS Readback")), "2.5e-09");

  std::lock_guard lock(rig.model->mutex);
  ASSERT_FALSE(rig.model->commands.empty());
  EXPECT_EQ(rig.model->commands.back(), "GetParameter Ion Gauge MS Readback");
}

TEST(QtegraLink, LinkNameDefaultsToTheDriverName) {
  Rig rig;
  auto owner = make_spectrometer(*rig.sim, "");
  ASSERT_TRUE(owner) << owner.error().what;
  EXPECT_TRUE(QtegraLinkRegistry::global().find("argus"));
}

TEST(QtegraLink, ASecondOwnerOfTheSameNameIsAConfigError) {
  Rig a, b;
  auto first = make_spectrometer(*a.sim, "link = \"one\"");
  ASSERT_TRUE(first) << first.error().what;
  auto second = make_spectrometer(*b.sim, "link = \"one\"");
  ASSERT_FALSE(second);
  EXPECT_EQ(second.error().kind, ErrorKind::Config);
  EXPECT_NE(second.error().what.find("already open"), std::string::npos) << second.error().what;
}

TEST(QtegraLink, TheSpectrometerMustOwnItsConnection) {
  LinkTransport borrowed("spec", "argus_link");
  auto dev = make_spectrometer(borrowed, "");
  ASSERT_FALSE(dev);
  EXPECT_EQ(dev.error().kind, ErrorKind::Config);
}

TEST(QtegraLink, ABorrowerOfAGoneOwnerIsNotConnected) {
  LinkTransport borrowed("qtegra_valves", "gone_link");
  auto handle = make_qtegra_link(borrowed, "unused", codec::qtegra::kDefaultTerminator, kClock);
  ASSERT_TRUE(handle);
  {
    Rig rig;
    auto owner = make_spectrometer(*rig.sim, "link = \"gone_link\"");
    ASSERT_TRUE(owner) << owner.error().what;
    EXPECT_TRUE(handle->get());
  }
  auto link = handle->get();
  ASSERT_FALSE(link);
  EXPECT_EQ(link.error().kind, ErrorKind::NotConnected);
}

TEST(QtegraLink, ABorrowersDroppedConnectionRunsTheOwnersHandshake) {
  Rig rig;
  Flaky flaky(*rig.sim);
  auto owner = make_spectrometer(flaky, "link = \"flaky_link\"");
  ASSERT_TRUE(owner) << owner.error().what;
  LinkTransport borrowed("qtegra_gauges", "flaky_link");
  auto handle = make_qtegra_link(borrowed, "unused", codec::qtegra::kDefaultTerminator, kClock);
  ASSERT_TRUE(handle);
  auto link = handle->get();
  ASSERT_TRUE(link);

  flaky.fail_exchanges = 1;
  const int opens = flaky.opens;
  EXPECT_EQ(text((*link)->ask("GetMagnetDAC")), "0");
  EXPECT_EQ(flaky.opens, opens + 1);
  EXPECT_EQ((*link)->reconnects(), 1u);
  std::lock_guard lock(rig.model->mutex);
  ASSERT_GE(rig.model->commands.size(), 2u);
  // Reopened, the owner's connect step, then the borrower's command.
  EXPECT_EQ(rig.model->commands[rig.model->commands.size() - 2], "GetIntegrationTime");
  EXPECT_EQ(rig.model->commands.back(), "GetMagnetDAC");
}

TEST(QtegraLink, CommandsFromOwnerAndBorrowerNeverTakeEachOthersReplies) {
  Rig rig;
  rig.model->dac = 3.0;
  rig.model->params["Valve 1"] = 7.0;
  auto owner = make_spectrometer(*rig.sim, "link = \"busy_link\"");
  ASSERT_TRUE(owner) << owner.error().what;
  auto* positioner = capability<IMassPositioner>(**owner);
  ASSERT_NE(positioner, nullptr);
  LinkTransport borrowed("qtegra_valves", "busy_link");
  auto handle = make_qtegra_link(borrowed, "unused", codec::qtegra::kDefaultTerminator, kClock);
  ASSERT_TRUE(handle);
  auto link = handle->get();
  ASSERT_TRUE(link);

  std::atomic<int> wrong{0};
  std::thread spectrometer_thread([&] {
    for (int i = 0; i < 200; ++i) {
      auto dac = positioner->read();
      if (!dac || *dac != 3.0) ++wrong;
    }
  });
  std::thread valve_thread([&] {
    for (int i = 0; i < 200; ++i) {
      auto reply = (*link)->ask("GetParameter Valve 1");
      if (!reply || !to_string(*reply).starts_with('7')) ++wrong;
    }
  });
  spectrometer_thread.join();
  valve_thread.join();
  EXPECT_EQ(wrong, 0);
}

TEST(QtegraLink, AnOwnerGoneMidUseNeverCallsBackIntoIt) {
  // A borrower holding the link past the owner's destruction must not run the
  // dead owner's handshake on reconnect.
  Rig rig;
  Flaky flaky(*rig.sim);
  std::shared_ptr<QtegraLink> kept;
  {
    auto owner = make_spectrometer(flaky, "link = \"kept_link\"");
    ASSERT_TRUE(owner) << owner.error().what;
    auto found = QtegraLinkRegistry::global().find("kept_link");
    ASSERT_TRUE(found);
    kept = *found;
  }
  flaky.fail_exchanges = 1;
  EXPECT_EQ(text(kept->ask("GetMagnetDAC")), "0");
  std::lock_guard lock(rig.model->mutex);
  EXPECT_EQ(rig.model->commands.back(), "GetMagnetDAC");
  for (const auto& c : rig.model->commands) EXPECT_NE(c, "GetIntegrationTime");
}
