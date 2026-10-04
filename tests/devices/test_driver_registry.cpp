#include "pychron/devices/driver_registry.hpp"

#include <gtest/gtest.h>

#include <algorithm>

#include "pychron/devices/capabilities.hpp"
#include "pychron/transport/sim_transport.hpp"

using namespace pychron;

namespace {

std::unique_ptr<SimTransport> silent_bus() {
  TransportOptions o;
  o.name = "bus";
  return SimTransport::scripted({}, o);
}

toml::table table_of(std::string_view text) {
  auto r = toml::parse(text, std::string_view("drivers.toml"));
  EXPECT_TRUE(r) << r.error().description();
  return r ? std::move(r).table() : toml::table{};
}

// Minimal vendor-free driver used to exercise the registry.
class EchoGauge : public Device, public IPressureGauge {
 public:
  EchoGauge(std::string name, Transport& t, double scale)
      : Device(std::move(name)), transport_(t), scale_(scale) {}

  static DriverSchema schema() {
    return {"", "test gauge that echoes a number",
            {{"scale", KeyType::Float, true, "multiplier applied to readings"},
             {"channels", KeyType::IntegerArray, false, "gauge channels"},
             {"label", KeyType::String, false, "free text"},
             {"verbose", KeyType::Boolean, false, ""},
             {"tags", KeyType::StringArray, false, ""},
             {"address", KeyType::Integer, false, ""}}};
  }

  static Result<std::unique_ptr<EchoGauge>> create(const DriverArgs& args) {
    double scale = args.options["scale"].value<double>().value_or(1.0);
    if (scale == 0.0) return fail(ErrorKind::Config, "scale must be non-zero");
    return std::make_unique<EchoGauge>(args.name, args.transport, scale);
  }

  Result<double> read_pressure() override { return scale_; }
  Transport& transport() { return transport_; }

 private:
  Transport& transport_;
  double scale_;
};

class RegistryTest : public ::testing::Test {
 protected:
  RegistryTest() : sim_(silent_bus()) {
    EXPECT_TRUE(reg_.add<EchoGauge>("echo_gauge"));
  }
  DriverRegistry reg_;
  std::unique_ptr<SimTransport> sim_;
};

}  // namespace

REGISTER_DRIVER("test_echo_gauge", EchoGauge);

TEST_F(RegistryTest, CreatesDriverWithNameTransportAndOptions) {
  auto t = table_of("kind = 'echo_gauge'\ntransport = 'bus'\nscale = 2.5\nchannels = [1, 2]\n");
  auto d = reg_.create("echo_gauge", *sim_, t, {"ig_controller", nullptr});
  ASSERT_TRUE(d) << to_string(d.error());
  EXPECT_EQ((*d)->name(), "ig_controller");
  auto* g = capability<IPressureGauge>(**d);
  ASSERT_NE(g, nullptr);
  EXPECT_DOUBLE_EQ(*g->read_pressure(), 2.5);
  EXPECT_EQ(&static_cast<EchoGauge&>(**d).transport(), sim_.get());
}

TEST_F(RegistryTest, NameDefaultsToKind) {
  auto d = reg_.create("echo_gauge", *sim_, table_of("scale = 1.0"));
  ASSERT_TRUE(d);
  EXPECT_EQ((*d)->name(), "echo_gauge");
}

TEST_F(RegistryTest, IntegerAcceptedForFloatKey) {
  EXPECT_TRUE(reg_.create("echo_gauge", *sim_, table_of("scale = 3")));
}

TEST_F(RegistryTest, UnknownKindIsConfigError) {
  auto d = reg_.create("nope", *sim_, table_of(""), {"x", nullptr});
  ASSERT_FALSE(d);
  EXPECT_EQ(d.error().kind, ErrorKind::Config);
  EXPECT_EQ(d.error().device, "x");
  EXPECT_NE(d.error().what.find("unknown driver kind 'nope'"), std::string::npos);
}

TEST_F(RegistryTest, MissingRequiredKey) {
  auto d = reg_.create("echo_gauge", *sim_, table_of("channels = [1]"));
  ASSERT_FALSE(d);
  EXPECT_EQ(d.error().kind, ErrorKind::Config);
  EXPECT_NE(d.error().what.find("missing required key 'scale'"), std::string::npos) << d.error().what;
}

TEST_F(RegistryTest, UndeclaredKeyRejectedWithLine) {
  auto d = reg_.create("echo_gauge", *sim_, table_of("scale = 1.0\nbaud = 9600\n"));
  ASSERT_FALSE(d);
  EXPECT_EQ(d.error().kind, ErrorKind::Config);
  EXPECT_NE(d.error().what.find("drivers.toml:2: undeclared key 'baud'"), std::string::npos)
      << d.error().what;
}

TEST_F(RegistryTest, CommonKeysAlwaysAllowed) {
  EXPECT_TRUE(reg_.create("echo_gauge", *sim_, table_of("kind='echo_gauge'\ntransport='bus'\nscale=1.0")));
}

TEST_F(RegistryTest, TypeMismatchesAllReported) {
  auto d = reg_.create("echo_gauge", *sim_,
                       table_of("scale = 'big'\nchannels = [1, 'two']\nlabel = 3\nverbose = 1\n"
                             "tags = [1]\naddress = 1.5\n"));
  ASSERT_FALSE(d);
  const std::string& w = d.error().what;
  for (const char* expect : {"'scale' must be a float", "'channels' must be an array of integers",
                             "'label' must be a string", "'verbose' must be a boolean",
                             "'tags' must be an array of strings", "'address' must be an integer"}) {
    EXPECT_NE(w.find(expect), std::string::npos) << expect << " in: " << w;
  }
}

TEST_F(RegistryTest, FactoryErrorsAreAttributedToDevice) {
  auto d = reg_.create("echo_gauge", *sim_, table_of("scale = 0.0"), {"ig1", nullptr});
  ASSERT_FALSE(d);
  EXPECT_EQ(d.error().kind, ErrorKind::Config);
  EXPECT_EQ(d.error().device, "ig1");
}

TEST_F(RegistryTest, ValidateWithoutCreating) {
  EXPECT_TRUE(reg_.validate("echo_gauge", table_of("scale = 1.0")));
  EXPECT_FALSE(reg_.validate("echo_gauge", table_of("")));
  EXPECT_FALSE(reg_.validate("missing", table_of("")));
}

TEST_F(RegistryTest, CreateFromDriverConfig) {
  config::DriverConfig c;
  c.name = "ig";
  c.kind = "echo_gauge";
  c.transport = "bus";
  c.options = table_of("kind='echo_gauge'\ntransport='bus'\nscale=4.0");
  auto d = reg_.create(c, *sim_);
  ASSERT_TRUE(d) << to_string(d.error());
  EXPECT_EQ((*d)->name(), "ig");
}

TEST_F(RegistryTest, DuplicateKindRejectedAndRecorded) {
  auto again = reg_.add<EchoGauge>("echo_gauge");
  ASSERT_FALSE(again);
  EXPECT_EQ(again.error().kind, ErrorKind::Config);
  ASSERT_EQ(reg_.conflicts().size(), 1u);
  EXPECT_EQ(reg_.conflicts()[0].kind, ErrorKind::Config);
}

TEST_F(RegistryTest, SchemasAreSortedAndCarryKind) {
  EXPECT_TRUE(reg_.add("a_first", DriverSchema{"", "first", {}},
                       [](const DriverArgs&) -> Result<std::unique_ptr<Device>> {
                         return fail(ErrorKind::Config, "unused");
                       }));
  EXPECT_EQ(reg_.kinds(), (std::vector<std::string>{"a_first", "echo_gauge"}));
  ASSERT_NE(reg_.schema("echo_gauge"), nullptr);
  EXPECT_EQ(reg_.schema("echo_gauge")->kind, "echo_gauge");
  EXPECT_EQ(reg_.schema("zzz"), nullptr);
  EXPECT_TRUE(reg_.contains("a_first"));
  EXPECT_FALSE(reg_.contains("zzz"));
}

TEST_F(RegistryTest, DescribeListsKeysForListDrivers) {
  std::string text = describe(*reg_.schema("echo_gauge"));
  EXPECT_NE(text.find("echo_gauge"), std::string::npos) << text;
  EXPECT_NE(text.find("test gauge that echoes a number"), std::string::npos);
  EXPECT_NE(text.find("scale"), std::string::npos);
  EXPECT_NE(text.find("float"), std::string::npos);
  EXPECT_NE(text.find("required"), std::string::npos);
  EXPECT_NE(text.find("array<integer>"), std::string::npos);
  EXPECT_NE(text.find("multiplier applied to readings"), std::string::npos);
}

TEST(DriverRegistryGlobal, RegisterDriverMacroAddsToGlobalRegistry) {
  EXPECT_TRUE(DriverRegistry::global().contains("test_echo_gauge"));
  auto sim = silent_bus();
  auto d = DriverRegistry::global().create("test_echo_gauge", *sim, table_of("scale = 1.0"));
  ASSERT_TRUE(d) << to_string(d.error());
}

// Registered in a separate static library; proves drivers linked through
// pychron_link_drivers() survive the linker even though nothing references them.
TEST(DriverRegistryGlobal, StaticLibraryRegistrationSurvivesLinking) {
  EXPECT_TRUE(DriverRegistry::global().contains("test_archived_driver"));
}

TEST(DriverRegistryGlobal, OnlyExtractionDevicesAreMarkedSo) {
  for (const auto& schema : DriverRegistry::global().schemas()) {
    EXPECT_EQ(schema.extraction_device, schema.kind == "chromium") << schema.kind;
  }
}

TEST(KeyTypeNames, Stable) {
  EXPECT_EQ(to_string(KeyType::String), "string");
  EXPECT_EQ(to_string(KeyType::Integer), "integer");
  EXPECT_EQ(to_string(KeyType::FloatArray), "array<float>");
  EXPECT_EQ(to_string(KeyType::Float), "float");
  EXPECT_EQ(to_string(KeyType::Boolean), "boolean");
  EXPECT_EQ(to_string(KeyType::IntegerArray), "array<integer>");
  EXPECT_EQ(to_string(KeyType::StringArray), "array<string>");
}
