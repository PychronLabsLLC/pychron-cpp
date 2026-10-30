#include "pychron/transport/endpoints.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace pychron;
namespace fs = std::filesystem;

namespace {

class SimEndpointsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() /
           ("pychron_endpoints_" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    fs::remove_all(dir_);
    fs::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }
  fs::path write(const std::string& name, const std::string& text) {
    const auto p = dir_ / name;
    std::ofstream(p) << text;
    return p;
  }
  fs::path dir_;
};

SimEndpoints sample(const fs::path& config) {
  SimEndpoints e;
  e.config = config;
  e.host = "127.0.0.1";
  e.transports["switch_controller"] = Endpoint{"127.0.0.1", 40213, WireProtocol::Tcp, "agilent_switch"};
  e.transports["gauges"] = Endpoint{"127.0.0.1", 40214, WireProtocol::Udp, ""};
  return e;
}

}  // namespace

TEST_F(SimEndpointsTest, RoundTrips) {
  const auto file = dir_ / "sim-endpoints.toml";
  const auto written = sample(dir_ / "extraction_line.toml");
  ASSERT_TRUE(write_sim_endpoints(file, written));
  auto read = read_sim_endpoints(file);
  ASSERT_TRUE(read) << read.error().what;
  EXPECT_EQ(*read, written);
}

TEST_F(SimEndpointsTest, EntryHostIsTheFilesHost) {
  const auto file = write("e.toml",
                          "config = \"/x/line.toml\"\nhost = \"10.0.0.5\"\n"
                          "[transports.a]\nprotocol = \"tcp\"\nport = 47000\n");
  auto read = read_sim_endpoints(file);
  ASSERT_TRUE(read) << read.error().what;
  EXPECT_EQ(read->transports.at("a").host, "10.0.0.5");
  EXPECT_EQ(read->transports.at("a").emulator, "");
}

TEST_F(SimEndpointsTest, UnknownProtocolIsConfigError) {
  const auto file = write("e.toml",
                          "config = \"/x/line.toml\"\nhost = \"127.0.0.1\"\n"
                          "[transports.a]\nprotocol = \"serial\"\nport = 47000\n");
  auto read = read_sim_endpoints(file);
  ASSERT_FALSE(read);
  EXPECT_EQ(read.error().kind, ErrorKind::Config);
  EXPECT_NE(read.error().what.find("serial"), std::string::npos) << read.error().what;
}

TEST_F(SimEndpointsTest, PortOutOfRangeIsConfigError) {
  for (const char* port : {"0", "65536"}) {
    const auto file = write("e.toml", std::string("config = \"/x/line.toml\"\nhost = \"127.0.0.1\"\n"
                                                  "[transports.a]\nprotocol = \"tcp\"\nport = ") +
                                          port + "\n");
    auto read = read_sim_endpoints(file);
    ASSERT_FALSE(read) << port;
    EXPECT_EQ(read.error().kind, ErrorKind::Config);
  }
}

TEST_F(SimEndpointsTest, MissingFileIsConfigErrorNamingIt) {
  auto read = read_sim_endpoints(dir_ / "absent.toml");
  ASSERT_FALSE(read);
  EXPECT_EQ(read.error().kind, ErrorKind::Config);
  EXPECT_NE(read.error().what.find("absent.toml"), std::string::npos) << read.error().what;
}

TEST_F(SimEndpointsTest, RefusesOtherConfig) {
  std::ofstream(dir_ / "line.toml") << "";
  std::ofstream(dir_ / "other.toml") << "";
  const auto file = dir_ / "sim-endpoints.toml";
  ASSERT_TRUE(write_sim_endpoints(file, sample(dir_ / "line.toml")));
  auto refused = load_sim_endpoints(file, dir_ / "other.toml", false);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().kind, ErrorKind::Config);
  EXPECT_NE(refused.error().what.find("--sim-endpoints-any"), std::string::npos) << refused.error().what;
  EXPECT_TRUE(load_sim_endpoints(file, dir_ / "other.toml", true));
  EXPECT_TRUE(load_sim_endpoints(file, dir_ / "line.toml", false));
}

TEST_F(SimEndpointsTest, SameConfigBySymlinkAccepted) {
  fs::create_directories(dir_ / "real");
  std::ofstream(dir_ / "real" / "line.toml") << "";
  std::error_code ec;
  fs::create_directory_symlink(dir_ / "real", dir_ / "link", ec);
  if (ec) GTEST_SKIP() << "cannot make a directory symlink here: " << ec.message();
  const auto file = dir_ / "sim-endpoints.toml";
  ASSERT_TRUE(write_sim_endpoints(file, sample(dir_ / "link" / "line.toml")));
  auto loaded = load_sim_endpoints(file, dir_ / "real" / ".." / "real" / "line.toml", false);
  EXPECT_TRUE(loaded) << loaded.error().what;
}

TEST(SimEndpoints, DefaultPathIsBesideTheLine) {
  EXPECT_EQ(default_sim_endpoints_path(fs::path("lab") / "extraction_line.toml"),
            fs::path("lab") / "sim-endpoints.toml");
}
