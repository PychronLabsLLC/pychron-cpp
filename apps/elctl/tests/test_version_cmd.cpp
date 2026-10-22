#include "version_cmd.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "fake_http.hpp"
#include "pychron/setup/installer.hpp"

namespace {

using pychron::update::testing::FakeHttp;
using pychron::update::testing::release_json;

struct Outcome {
  int code = 0;
  std::string out, err;
};

Outcome run(const std::vector<std::string>& args, FakeHttp& fake) {
  std::istringstream in;
  std::ostringstream out, err;
  Outcome o;
  o.code = elctl::version_command(args, elctl::Io{in, out, err}, elctl::VersionDeps{[&] { return fake.borrow(); }});
  o.out = out.str();
  o.err = err.str();
  return o;
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

}  // namespace

TEST(VersionCmd, PlainPrintsVersionAndSendsNothing) {
  FakeHttp fake;
  const Outcome o = run({}, fake);
  EXPECT_EQ(o.code, elctl::kOk);
  EXPECT_EQ(o.out.rfind("elctl ", 0), 0U) << o.out;
  EXPECT_TRUE(fake.sent().empty());
}

TEST(VersionCmd, UpToDate) {
  FakeHttp fake;
  fake.answer(200, release_json("v" + std::string(pychron::setup::version())));
  const Outcome o = run({"--check"}, fake);
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_TRUE(contains(o.out, "(up to date)")) << o.out;
}

TEST(VersionCmd, Newer) {
  FakeHttp fake;
  fake.answer(200, release_json("v999.0.0"));
  const Outcome o = run({"--check"}, fake);
  EXPECT_EQ(o.code, elctl::kUpdateAvailable) << o.err;
  EXPECT_TRUE(contains(o.out, "latest: 999.0.0")) << o.out;
  EXPECT_TRUE(contains(o.out, "download: https://")) << o.out;
}

TEST(VersionCmd, NoRelease) {
  FakeHttp fake;
  fake.answer(404, "{}");
  const Outcome o = run({"--check"}, fake);
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_TRUE(contains(o.out, "none published")) << o.out;
}

TEST(VersionCmd, BuildIsNewer) {
  FakeHttp fake;
  fake.answer(200, release_json("v0.0.1"));
  const Outcome o = run({"--check"}, fake);
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_TRUE(contains(o.out, "this build is newer")) << o.out;
}

TEST(VersionCmd, Failure) {
  FakeHttp fake;
  fake.fail(pychron::ErrorKind::Io);
  const Outcome o = run({"--check"}, fake);
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "could not check for updates")) << o.err;
}

TEST(VersionCmd, UnknownArgument) {
  FakeHttp fake;
  const Outcome o = run({"--chek"}, fake);
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(fake.sent().empty());
}

TEST(VersionCmd, NoClient) {
  std::istringstream in;
  std::ostringstream out, err;
  const int code = elctl::version_command({"--check"}, elctl::Io{in, out, err},
                                          elctl::VersionDeps{[] { return std::unique_ptr<pychron::http::IHttpClient>{}; }});
  EXPECT_EQ(code, elctl::kUsage);
  EXPECT_TRUE(contains(err.str(), "this build has no network client")) << err.str();
}
