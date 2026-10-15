// Which links a run needs, and the pre-run checks that ask the queue to wait
// (executor hardening spec, sections 4.3 and 4.6).

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/experiment/executor/links.hpp"
#include "pychron/transport/sim_transport.hpp"

using namespace pychron;
using namespace pychron::experiment;
using namespace pychron::experiment::executor;

namespace {

// A transport and its link, opened: Connected until it is closed.
struct Wire {
  Wire(const std::string& name, const Clock& clock, bool optional = false) {
    TransportOptions o;
    o.name = name;
    o.clock = &clock;
    transport = SimTransport::hooked([](const Bytes&) { return Bytes{}; }, o);
    EXPECT_TRUE(transport->open());
    LinkPolicy p;
    p.optional = optional;
    link = std::make_unique<Link>(*transport, clock, p);
  }
  std::unique_ptr<SimTransport> transport;
  std::unique_ptr<Link> link;
};

RunSpec measured_run() {
  RunSpec r;
  r.id.identifier = "12345";
  r.measurement.plan = "mc";
  return r;
}

std::vector<std::string> names(const LinkSet& s) {
  std::vector<std::string> out;
  out.reserve(s.required.size());
  for (Link* l : s.required) out.push_back(l->transport().name());
  return out;
}

}  // namespace

class LinkSetTest : public ::testing::Test {
 protected:
  ManualClock clock;
  Wire valve_bus{"valve_bus", clock}, co2{"co2", clock}, gauge_net{"gauge_net", clock, /*optional=*/true};
  Wire spec_dac{"dac", clock}, spec_counter{"counter", clock};
  std::vector<Link*> line{valve_bus.link.get(), co2.link.get(), gauge_net.link.get()};
  std::vector<Link*> spectrometer{spec_dac.link.get(), spec_counter.link.get()};
};

// The whole line (its extraction device, co2 here, is a driver on it) and
// the spectrometer.
TEST_F(LinkSetTest, ARunNeedsTheWholeLineItsDeviceAndTheSpectrometer) {
  EXPECT_EQ(names(required_links(measured_run(), line, spectrometer)),
            (std::vector<std::string>{"valve_bus", "co2", "dac", "counter"}));
}

TEST_F(LinkSetTest, ARunWithoutAPlanDoesNotNeedTheSpectrometer) {
  RunSpec degas = measured_run();
  degas.measurement.plan.clear();
  EXPECT_EQ(names(required_links(degas, line, spectrometer)), (std::vector<std::string>{"valve_bus", "co2"}));
}

TEST_F(LinkSetTest, AnOptionalTransportIsLeftOut) {
  for (Link* l : required_links(measured_run(), line, spectrometer).required)
    EXPECT_NE(l->transport().name(), "gauge_net");
}

TEST_F(LinkSetTest, LinksUpWaitsWhenARequiredLinkIsDown) {
  LinksUp check([this](const RunSpec& r) { return required_links(r, line, spectrometer); });
  EXPECT_FALSE(check.check(measured_run()));
  spec_counter.transport->close();
  valve_bus.transport->close();
  const auto r = check.check(measured_run());
  ASSERT_TRUE(r);
  EXPECT_EQ(r->kind, CheckKind::Wait);
  EXPECT_EQ(r->links, (std::vector<std::string>{"valve_bus", "counter"}));
  EXPECT_NE(r->error.what.find("links down: valve_bus"), std::string::npos) << r->error.what;
}

TEST_F(LinkSetTest, LinksUpPassesWhenOnlyAnOptionalOneIsDown) {
  LinksUp check([this](const RunSpec& r) { return required_links(r, line, spectrometer); });
  gauge_net.transport->close();
  EXPECT_FALSE(check.check(measured_run()));
}

TEST(SpoolWritable, WaitsUnderMinFreeOrOverMaxPending) {
  double free_mb = 10000;
  std::size_t pending = 0;
  SpoolWritable check({500, 200}, [&] { return std::optional<double>(free_mb); }, [&] { return pending; });
  EXPECT_FALSE(check.check(measured_run()));
  free_mb = 499;
  auto low = check.check(measured_run());
  ASSERT_TRUE(low);
  EXPECT_EQ(low->kind, CheckKind::Wait);
  EXPECT_NE(low->error.what.find("499 MB free, under 500 MB"), std::string::npos) << low->error.what;
  free_mb = 500;
  pending = 200;
  EXPECT_FALSE(check.check(measured_run()));
  pending = 201;
  auto backlog = check.check(measured_run());
  ASSERT_TRUE(backlog);
  EXPECT_EQ(backlog->kind, CheckKind::Wait);
  EXPECT_NE(backlog->error.what.find("201 records"), std::string::npos) << backlog->error.what;
}

TEST(SpoolWritable, UnknownFreeSpaceIsNoReasonToWait) {
  SpoolWritable check({500, 200}, [] { return std::optional<double>{}; }, [] { return std::size_t{0}; });
  EXPECT_FALSE(check.check(measured_run()));
}
