#include <gtest/gtest.h>

#include "extraction/fake.hpp"
#include "pychron/devices/extraction/capability.hpp"
#include "pychron/devices/extraction/interfaces.hpp"
#include "pychron/devices/extraction/services.hpp"

namespace pychron::extraction {
namespace {

TEST(ExtractionCapability, NamesRoundTrip) {
  for (std::size_t i = 0; i < kCapabilityCount; ++i) {
    auto c = static_cast<Capability>(i);
    auto name = to_string(c);
    EXPECT_FALSE(name.empty());
    EXPECT_EQ(capability_from_string(name), c) << name;
  }
  EXPECT_EQ(to_string(Capability::Pattern), "pattern");
  EXPECT_EQ(capability_from_string("warp_drive"), std::nullopt);
}

TEST(ExtractionCapability, SetAddRemoveListInDeclarationOrder) {
  CapabilitySet set{Capability::Stage, Capability::Laser};
  EXPECT_TRUE(set.has(Capability::Laser));
  EXPECT_FALSE(set.has(Capability::Furnace));
  EXPECT_EQ(set.size(), 2u);
  EXPECT_EQ(set.list(), (std::vector<Capability>{Capability::Laser, Capability::Stage}));
  set.remove(Capability::Laser);
  set.remove(Capability::Stage);
  EXPECT_TRUE(set.empty());
}

TEST(ExtractionCapability, NotSupportedIsATaggedConfigError) {
  auto e = not_supported(Capability::Cryo, "furnace1");
  EXPECT_EQ(e.kind, ErrorKind::Config);
  EXPECT_EQ(e.device, "furnace1");
  EXPECT_EQ(e.what, "not supported: cryo");
  EXPECT_TRUE(is_not_supported(e));
  EXPECT_TRUE(is_not_supported(not_supported("extract in watts")));
  EXPECT_FALSE(is_not_supported(Error{ErrorKind::Config, "bad value", ""}));
  EXPECT_FALSE(is_not_supported(Error{ErrorKind::Io, "not supported: x", ""}));
}

TEST(ExtractionUnits, NamesMatchPychronVocabulary) {
  EXPECT_EQ(extract_units_from_string("percent"), ExtractUnits::Percent);
  EXPECT_EQ(extract_units_from_string("watts"), ExtractUnits::Watts);
  EXPECT_EQ(extract_units_from_string("temp"), ExtractUnits::Celsius);
  EXPECT_EQ(extract_units_from_string("kelvin"), std::nullopt);
  EXPECT_EQ(to_string(ExtractUnits::Celsius), "temp");
}

TEST(ExtractionCapability, DeviceWithoutFeaturesHasNone) {
  testing::FakeExtractionDevice device("bare");
  EXPECT_TRUE(capabilities(device).empty());
}

TEST(ExtractionCapability, DeviceReportsEachExposedFeature) {
  testing::FakeExtractionDevice device("laser1", {Capability::Laser, Capability::Stage});
  EXPECT_EQ(capabilities(device), (CapabilitySet{Capability::Laser, Capability::Stage}));
  EXPECT_NE(device.laser(), nullptr);
  EXPECT_NE(device.stage(), nullptr);
  EXPECT_EQ(device.furnace(), nullptr);
}

TEST(ExtractionCapability, ServicesAddValvesAndPressure) {
  testing::FakeExtractionDevice device("f", {Capability::Furnace});
  testing::FakeValveService valves({"A", "B"});
  ExtractionServices services{&device, &valves, nullptr};
  EXPECT_EQ(capabilities(services), (CapabilitySet{Capability::Furnace, Capability::Valves}));
  EXPECT_TRUE(capabilities(ExtractionServices{}).empty());
}

TEST(ExtractionCapability, ServicesIgnoreDeviceClaimsForLineServices) {
  // Pressure/Valves come only from the service pointers, never the device.
  testing::FakeExtractionDevice device("f", {Capability::Pressure, Capability::Valves});
  EXPECT_TRUE(capabilities(ExtractionServices{&device, nullptr, nullptr}).empty());
}

}  // namespace
}  // namespace pychron::extraction
