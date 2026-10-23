// Commands that never touch a transport: validate, canvas-check,
// list-drivers, list, help and usage errors.

#include <fstream>
#include <iterator>

#include "elctl_fixture.hpp"

namespace elctl::testing {
namespace {

TEST_F(ElctlTest, ValidateExampleSucceeds) {
  auto o = run_raw({"validate", config()});
  EXPECT_EQ(o.code, 0) << o.err;
  EXPECT_TRUE(contains(o.out, "ok")) << o.out;
}

TEST_F(ElctlTest, ValidateDefaultsToConfigOption) {
  auto o = run({"validate"});
  EXPECT_EQ(o.code, 0) << o.err;
}

TEST_F(ElctlTest, ValidatePrintsEveryErrorWithLocation) {
  write("bad.toml", R"([system]
name = "bad"

[transports.bus]
kind = "sim"

[drivers.act]
kind = "proxr_relay"
transport = "bus"

[[valves]]
name = "A"
actuator = "nope"
address = "1"

[[valves]]
name = "B"
actuator = "act"
address = "2"
interlocks = ["ghost"]
)");
  auto o = run_raw({"validate", path("bad.toml").string()});
  EXPECT_EQ(o.code, 1);
  EXPECT_TRUE(contains(o.err, "bad.toml:13")) << o.err;
  EXPECT_TRUE(contains(o.err, "nope")) << o.err;
  EXPECT_TRUE(contains(o.err, "ghost")) << o.err;
}

TEST_F(ElctlTest, ValidateChecksDriverKeysAgainstRegistry) {
  write("drivers.toml", R"([system]
name = "d"

[transports.bus]
kind = "sim"

[drivers.x]
kind = "no_such_driver"
transport = "bus"

[drivers.ig]
kind = "pfeiffer_maxigauge"
transport = "bus"
channels = [1]
bogus = 3
)");
  auto o = run_raw({"validate", path("drivers.toml").string()});
  EXPECT_EQ(o.code, 1);
  EXPECT_TRUE(contains(o.err, "no_such_driver")) << o.err;
  EXPECT_TRUE(contains(o.err, "bogus")) << o.err;
}

TEST_F(ElctlTest, ValidateMissingFileFails) {
  auto o = run_raw({"validate", path("missing.toml").string()});
  EXPECT_EQ(o.code, 1);
  EXPECT_FALSE(o.err.empty());
}

TEST_F(ElctlTest, CanvasCheckExampleSucceeds) {
  auto o = run({"canvas-check", path("canvas.toml").string()});
  EXPECT_EQ(o.code, 0) << o.err;
  EXPECT_TRUE(contains(o.out, "ok")) << o.out;
}

TEST_F(ElctlTest, CanvasCheckDefaultsToSiblingCanvas) {
  auto o = run({"canvas-check"});
  EXPECT_EQ(o.code, 0) << o.err;
}

TEST_F(ElctlTest, CanvasCheckReportsUnknownValve) {
  write("bad_canvas.toml", R"([canvas]
size = [100, 100]

[[valve]]
name = "Z"
pos = [1, 1]

[[stage]]
name = "s"
pos = [5, 5]

[[connection]]
start = "Z"
end = "s"
)");
  auto o = run({"canvas-check", path("bad_canvas.toml").string()});
  EXPECT_EQ(o.code, 1);
  EXPECT_TRUE(contains(o.err, "Z")) << o.err;
}

TEST_F(ElctlTest, CanvasCheckWarnsAboutUndrawnValves) {
  write("sparse.toml", R"([canvas]
size = [100, 100]

[[valve]]
name = "A"
pos = [1, 1]
)");
  auto o = run({"canvas-check", path("sparse.toml").string()});
  EXPECT_EQ(o.code, 0) << o.err;
  EXPECT_TRUE(contains(o.out, "warning")) << o.out;
  EXPECT_TRUE(contains(o.out, "B")) << o.out;
}

TEST(ElctlOffline, ListDriversPrintsKindsAndKeys) {
  auto o = run_raw({"list-drivers"});
  EXPECT_EQ(o.code, 0);
  EXPECT_TRUE(contains(o.out, "proxr_relay")) << o.out;
  EXPECT_TRUE(contains(o.out, "pfeiffer_maxigauge")) << o.out;
  EXPECT_TRUE(contains(o.out, "channels")) << o.out;
}

TEST_F(ElctlTest, ListShowsEveryConfiguredItem) {
  auto o = run({"list"});
  EXPECT_EQ(o.code, 0) << o.err;
  for (const char* name : {"A", "B", "C", "P1", "P2", "M1", "pump_power", "IG1", "PG1"}) {
    EXPECT_TRUE(contains(o.out, name)) << name << "\n" << o.out;
  }
  EXPECT_TRUE(contains(o.out, "manual")) << o.out;
  EXPECT_TRUE(contains(o.out, "switch")) << o.out;
}

TEST_F(ElctlTest, ListWithInvalidConfigFails) {
  write("extraction_line.toml", "[system]\nname = 3\n");
  auto o = run({"list"});
  EXPECT_EQ(o.code, 1);
  EXPECT_TRUE(contains(o.err, "extraction_line.toml")) << o.err;
}

TEST(ElctlOffline, HelpListsCommands) {
  auto o = run_raw({"help"});
  EXPECT_EQ(o.code, 0);
  for (const char* cmd : {"validate", "canvas-check", "list-drivers", "probe", "list", "state", "open", "close",
                          "read", "scan", "trace", "sim"}) {
    EXPECT_TRUE(contains(o.out, cmd)) << cmd;
  }
}

TEST(ElctlOffline, NoCommandIsUsageError) {
  auto o = run_raw({});
  EXPECT_EQ(o.code, 2);
  EXPECT_TRUE(contains(o.err, "usage")) << o.err;
}

TEST(ElctlOffline, UnknownCommandIsUsageError) {
  auto o = run_raw({"frobnicate"});
  EXPECT_EQ(o.code, 2);
  EXPECT_TRUE(contains(o.err, "frobnicate")) << o.err;
}

TEST_F(ElctlTest, MissingArgumentIsUsageError) {
  EXPECT_EQ(run({"open"}).code, 2);
  EXPECT_EQ(run({"read"}).code, 2);
  EXPECT_EQ(run({"scan"}).code, 2);
  EXPECT_EQ(run({"scan", "--for", "soon"}).code, 2);
  EXPECT_EQ(run({"trace", "sideways"}).code, 2);
  EXPECT_EQ(run_raw({"-c"}).code, 2);
}

TEST_F(ElctlTest, ConditionalsCheckExampleSystemFile) {
  const auto examples = std::filesystem::path(PYCHRON_EXAMPLE_CONFIGS_DIR);
  auto o = run({"conditionals-check", (examples / "conditionals" / "system.toml").string(), "--spectrometer",
                (examples / "spectrometer.sim-integrated.toml").string()});
  EXPECT_EQ(o.code, 0) << o.err;
  EXPECT_TRUE(contains(o.out, "truncation huge_signal: Ar40.cur > 4000000")) << o.out;
  EXPECT_TRUE(contains(o.out, "ok: ")) << o.out;
}

TEST_F(ElctlTest, ConditionalsCheckReportsUnknownNamesAndBadSyntax) {
  const auto examples = std::filesystem::path(PYCHRON_EXAMPLE_CONFIGS_DIR);
  write("bad.toml", "[[terminations]]\ncheck = \"gauge.nope.pressure > 1 or IC9.inactive or Ar99 > 1\"\n");
  auto o = run({"conditionals-check", path("bad.toml").string(), "--spectrometer",
                (examples / "spectrometer.sim-integrated.toml").string()});
  EXPECT_EQ(o.code, 1);
  EXPECT_TRUE(contains(o.err, "unknown gauge 'nope'")) << o.err;
  EXPECT_TRUE(contains(o.err, "unknown detector 'IC9'")) << o.err;
  EXPECT_TRUE(contains(o.err, "unknown isotope 'Ar99'")) << o.err;
  write("syntax.toml", "[[terminations]]\ncheck = \"Ar40 >\"\n");
  EXPECT_EQ(run({"conditionals-check", path("syntax.toml").string()}).code, 1);
  EXPECT_EQ(run({"conditionals-check"}).code, 2);
}

class Cli : public ElctlTest {};

// The domain a command runs on: the install's `domain`, or --domain over it.
// What a session's domain is shows in `flux`, which runs on arar only and
// names the domain it was refused on.
TEST_F(Cli, DomainFlagAndInstallEntry) {
  // An id no domain has is a usage error that names the ones there are.
  const Outcome unknown = run({"--domain", "u_pb", "list-drivers"});
  EXPECT_EQ(unknown.code, kUsage);
  EXPECT_TRUE(contains(unknown.err, "unknown domain 'u_pb'")) << unknown.err;
  EXPECT_TRUE(contains(unknown.err, "known: arar, noble_gas")) << unknown.err;
  EXPECT_EQ(run({"--domain"}).code, kUsage);

  // A known one is accepted for a command that needs none.
  EXPECT_EQ(run({"--domain", "noble_gas", "list-drivers"}).code, kOk);
  EXPECT_EQ(run({"--domain", "arar", "list-drivers"}).code, kOk);

#ifdef PYCHRON_ELCTL_HAS_STORE
  const std::string refused = "elctl flux: needs domain arar (this install: ";
  const Outcome flag = run_raw({"--domain", "noble_gas", "flux", "fit", "NM-300", "A", "--db", "sqlite::memory:"});
  EXPECT_EQ(flag.code, kUsage);
  EXPECT_TRUE(contains(flag.err, refused + "noble_gas)")) << flag.err;

  // The install's entry names the domain; --domain overrides it.
  ASSERT_EQ(run_raw({"init", "data-reduction", "--root", path("lab").string(), "--name", "lab", "--yes"}).code, kOk);
  {
    std::ifstream in(path("site.toml"));
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto at = text.find("domain = ");
    ASSERT_NE(at, std::string::npos) << text;
    text.replace(at, text.find('\n', at) - at, "domain = 'noble_gas'");
    std::ofstream(path("site.toml"), std::ios::trunc) << text;
  }
  const Outcome entry = run_raw({"flux", "fit", "NM-300", "A", "--db", "sqlite::memory:"});
  EXPECT_EQ(entry.code, kUsage);
  EXPECT_TRUE(contains(entry.err, refused + "noble_gas)")) << entry.err;
  const Outcome overridden = run_raw({"--domain", "arar", "flux", "fit", "NM-300", "A", "--db", "sqlite::memory:"});
  EXPECT_FALSE(contains(overridden.err, "needs domain arar")) << overridden.err;
#endif
}

}  // namespace
}  // namespace elctl::testing
