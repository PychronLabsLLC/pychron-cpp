// qtegra_gauges: gauge readbacks through Qtegra (plan 2026-10-05, task B2).
#include "pychron/devices/spectrometer/qtegra_gauges.hpp"

#include <gtest/gtest.h>

#include "pychron/devices/driver_registry.hpp"
#include "pychron/devices/spectrometer/thermo_qtegra_sim.hpp"
#include "pychron/transport/link_transport.hpp"
#include "spectrometer/legacy/sim_util.hpp"

using namespace pychron;
using namespace pychron::spectrometer;

namespace {

Result<std::unique_ptr<Device>> make(Transport& t, std::string_view toml, std::string name = "qtegra_gauges") {
  return DriverRegistry::global().create("qtegra_gauges", t, legacy_test::table_of(toml), DriverContext{name});
}

}  // namespace

TEST(QtegraGauges, ChannelNReadsTheNthParameter) {
  auto model = std::make_shared<QtegraSimModel>();
  model->params["Ion Gauge MS Readback"] = 3.2e-9;
  model->params["Pirani Readback"] = 1.1e-2;
  auto sim = legacy_test::open_hooked(qtegra_sim_hook(model));
  auto dev = make(*sim, R"(parameters = ["Ion Gauge MS Readback", "Pirani Readback"]
link = "qg_own")");
  ASSERT_TRUE(dev) << dev.error().what;
  auto* gauge = capability<IChannelPressureGauge>(**dev);
  ASSERT_NE(gauge, nullptr);
  EXPECT_EQ(gauge->pressure_channels(), (std::vector<int>{1, 2}));
  EXPECT_DOUBLE_EQ(*gauge->read_pressure(1), 3.2e-9);
  EXPECT_DOUBLE_EQ(*gauge->read_pressure(2), 1.1e-2);
  std::lock_guard lock(model->mutex);
  EXPECT_EQ(model->commands.back(), "GetParameter Pirani Readback");
}

TEST(QtegraGauges, ShareTheSpectrometersConnection) {
  auto model = std::make_shared<QtegraSimModel>();
  model->params["Ion Gauge MS Readback"] = 4e-9;
  auto sim = legacy_test::open_hooked(qtegra_sim_hook(model));
  auto spectrometer = DriverRegistry::global().create("thermo_qtegra", *sim, legacy_test::table_of("link = \"ldeo\""),
                                                      DriverContext{"helix"});
  ASSERT_TRUE(spectrometer) << spectrometer.error().what;
  LinkTransport borrowed("gauges", "ldeo");
  auto dev = make(borrowed, R"(parameters = ["Ion Gauge MS Readback"])");
  ASSERT_TRUE(dev) << dev.error().what;
  EXPECT_DOUBLE_EQ(*capability<IPressureGauge>(**dev)->read_pressure(), 4e-9);
}

TEST(QtegraGauges, AnErrorOrNonsenseIsNeverAReading) {
  auto sim = legacy_test::open_hooked([](const Bytes&) { return to_bytes("ERROR: unknown parameter\r\n"); });
  auto dev = make(*sim, R"(parameters = ["Nope"]
link = "qg_err")");
  ASSERT_TRUE(dev);
  auto r = capability<IPressureGauge>(**dev)->read_pressure();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  EXPECT_EQ(r.error().device, "qtegra_gauges");
  auto negative = legacy_test::open_hooked([](const Bytes&) { return to_bytes("-1e-9\r\n"); });
  auto dev2 = make(*negative, R"(parameters = ["IG"]
link = "qg_neg")");
  ASSERT_TRUE(dev2);
  EXPECT_FALSE(capability<IPressureGauge>(**dev2)->read_pressure());
}

TEST(QtegraGauges, ConfigIsChecked) {
  auto sim = legacy_test::open_hooked([](const Bytes&) { return Bytes{}; });
  EXPECT_FALSE(make(*sim, "link = \"qg_c1\""));                                  // parameters required
  EXPECT_FALSE(make(*sim, "parameters = []\nlink = \"qg_c2\""));
  EXPECT_FALSE(make(*sim, "parameters = [\"A,B\"]\nlink = \"qg_c3\""));          // a comma
  EXPECT_FALSE(make(*sim, "parameters = [\"A\", \"A\"]\nlink = \"qg_c4\""));
  auto one = make(*sim, "parameters = [\"A\"]\nlink = \"qg_c5\"");
  ASSERT_TRUE(one);
  auto r = capability<IChannelPressureGauge>(**one)->read_pressure(2);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
}
