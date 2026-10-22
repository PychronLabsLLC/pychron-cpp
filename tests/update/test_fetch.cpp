#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include <pychron/update/update.hpp>

#include "fake_http.hpp"

namespace pychron::update {
namespace {

using testing::FakeHttp;
using testing::release_json;

constexpr const char* kUrl = "https://api.github.com/repos/PychronLabsLLC/pychron-cpp/releases/latest";

std::string header(const http::HttpRequest& request, const std::string& name) {
  for (const auto& [key, value] : request.headers) {
    if (key == name) return value;
  }
  return {};
}

TEST(UpdateFetch, AsksTheRightThing) {
  FakeHttp fake;
  fake.answer(200, release_json("v0.5.0"));
  ASSERT_TRUE(fetch_latest(fake).has_value());
  const auto sent = fake.sent();
  ASSERT_EQ(sent.size(), 1U);
  EXPECT_EQ(sent[0].method, "GET");
  EXPECT_EQ(sent[0].url, kUrl);
  EXPECT_EQ(sent[0].headers.size(), 2U);
  EXPECT_EQ(header(sent[0], "Accept"), "application/vnd.github+json");
  EXPECT_EQ(header(sent[0], "User-Agent").rfind("pychron-cpp/", 0), 0U);
  EXPECT_EQ(sent[0].timeout, std::chrono::seconds(10));
  EXPECT_TRUE(sent[0].body.empty());
}

TEST(UpdateFetch, OtherRepository) {
  FakeHttp fake;
  fake.answer(404, "");
  ASSERT_TRUE(fetch_latest(fake, "a/b").has_value());
  ASSERT_EQ(fake.sent().size(), 1U);
  EXPECT_EQ(fake.sent()[0].url, "https://api.github.com/repos/a/b/releases/latest");
}

TEST(UpdateFetch, FoundOn200) {
  FakeHttp fake;
  fake.answer(200, release_json("v0.5.0"));
  const auto got = fetch_latest(fake);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->outcome, FetchOutcome::Found);
  ASSERT_TRUE(got->release.has_value());
  EXPECT_EQ(got->release->version, (Version{0, 5, 0}));
}

TEST(UpdateFetch, NoReleaseOn404) {
  FakeHttp fake;
  fake.answer(404, "{}");
  const auto got = fetch_latest(fake);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->outcome, FetchOutcome::NoRelease);
  EXPECT_FALSE(got->release.has_value());
}

TEST(UpdateFetch, FailsOnOtherStatus) {
  for (const int status : {403, 429, 500}) {
    FakeHttp fake;
    fake.answer(status, "");
    const auto got = fetch_latest(fake);
    ASSERT_FALSE(got.has_value()) << status;
    EXPECT_NE(got.error().what.find(std::to_string(status)), std::string::npos) << got.error().what;
  }
}

TEST(UpdateFetch, FailsOnUnparsable200) {
  FakeHttp fake;
  fake.answer(200, "{");
  EXPECT_FALSE(fetch_latest(fake).has_value());
}

TEST(UpdateFetch, PassesClientErrorsThrough) {
  FakeHttp fake;
  fake.fail(ErrorKind::Io);
  fake.fail(ErrorKind::Timeout);
  const auto io = fetch_latest(fake);
  ASSERT_FALSE(io.has_value());
  EXPECT_EQ(io.error().kind, ErrorKind::Io);
  const auto timeout = fetch_latest(fake);
  ASSERT_FALSE(timeout.has_value());
  EXPECT_EQ(timeout.error().kind, ErrorKind::Timeout);
}

}  // namespace
}  // namespace pychron::update
