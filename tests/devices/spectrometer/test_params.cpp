#include <gtest/gtest.h>

#include <set>

#include "pychron/devices/spectrometer/params.hpp"
#include "spectrometer/fakes.hpp"

namespace {

using namespace pychron;
using namespace pychron::spectrometer;

TEST(SourceParamRegistry, CoversEveryParamWithUniqueNames) {
  auto all = source_params();
  EXPECT_EQ(all.size(), 19U);
  std::set<std::string_view> names;
  for (const auto& p : all) {
    EXPECT_FALSE(p.name.empty());
    EXPECT_TRUE(names.insert(p.name).second) << p.name;
    EXPECT_EQ(&info(p.param), &p);
  }
}

TEST(SourceParamRegistry, CanonicalNamesAndUnits) {
  EXPECT_EQ(to_string(SourceParam::HV), "hv");
  EXPECT_EQ(to_string(SourceParam::YSymmetry), "y_symmetry");
  EXPECT_EQ(to_string(SourceParam::ESAMinus), "esa_minus");
  EXPECT_EQ(info(SourceParam::TrapCurrent).unit, Unit::MicroAmps);
  EXPECT_EQ(info(SourceParam::ElectronEnergy).unit, Unit::ElectronVolts);
  EXPECT_EQ(to_string(Unit::Volts), "V");
  EXPECT_EQ(to_string(Unit::None), "");
}

TEST(ParamId, CanonicalRoundTrip) {
  for (const auto& p : source_params()) {
    auto parsed = parse_param_id(to_string(ParamId{p.param}));
    ASSERT_TRUE(parsed.has_value()) << p.name;
    EXPECT_EQ(*parsed, ParamId{p.param});
  }
}

TEST(ParamId, CustomRoundTripAndDistinctFromCanonical) {
  ParamId id{Custom{"Aux Lens"}};
  EXPECT_EQ(to_string(id), "custom:Aux Lens");
  auto parsed = parse_param_id("custom:Aux Lens");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(*parsed, id);
  EXPECT_NE(ParamId{Custom{"hv"}}, ParamId{SourceParam::HV});
}

TEST(ParamId, UnknownOrEmptyCustomIsConfigError) {
  auto unknown = parse_param_id("warp_drive");
  ASSERT_FALSE(unknown.has_value());
  EXPECT_EQ(unknown.error().kind, ErrorKind::Config);
  auto empty = parse_param_id("custom:");
  ASSERT_FALSE(empty.has_value());
  EXPECT_EQ(empty.error().kind, ErrorKind::Config);
}

TEST(ParamSpec, LookupByIdAndVendorName) {
  auto src = fakes::FakeSource::vendor();
  auto specs = src.params();
  const auto* ysym = find_spec(specs, ParamId{SourceParam::YSymmetry});
  ASSERT_NE(ysym, nullptr);
  EXPECT_EQ(ysym->vendor_name, "Y-Symmetry Set");
  EXPECT_EQ(find_vendor(specs, "Y-Symmetry Set"), ysym);
  EXPECT_EQ(find_spec(specs, ParamId{SourceParam::PoleN}), nullptr);
  EXPECT_EQ(find_vendor(specs, "YF"), nullptr);

  const auto* aux = find_spec(specs, ParamId{Custom{"Aux Lens"}});
  ASSERT_NE(aux, nullptr);
  EXPECT_EQ(aux->vendor_name, "Aux Lens Set");
}

TEST(ParamSpec, LegacySupplyAdvertisesOnlyHv) {
  auto src = fakes::FakeSource::hv_only();
  ASSERT_EQ(src.params().size(), 1U);
  EXPECT_EQ(src.params()[0].id, ParamId{SourceParam::HV});
}

TEST(Readback, ActualIsOptional) {
  Readback setpoint_only{5.0, std::nullopt};
  EXPECT_FALSE(setpoint_only.actual.has_value());
  Readback both{5.0, 4.99};
  EXPECT_NE(setpoint_only, both);
  EXPECT_DOUBLE_EQ(*both.actual, 4.99);
}

}  // namespace
