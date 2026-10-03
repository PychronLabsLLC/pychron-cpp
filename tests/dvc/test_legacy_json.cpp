// The tolerant reader: Python's NaN/Infinity tokens, BOM, CRLF, and the
// conversions the parsers rely on.

#include <gtest/gtest.h>

#include "legacy_json.hpp"

namespace pychron::dvc {
namespace {

TEST(LegacyJson, NanAndInfinityBecomeNull) {
  auto j = parse_legacy(R"({"a": NaN, "b": [Infinity, -Infinity], "c": "NaN"})");
  ASSERT_TRUE(j.has_value()) << j.error().what;
  EXPECT_TRUE((*j)["a"].is_null());
  ASSERT_EQ((*j)["b"].size(), 2u);
  EXPECT_TRUE((*j)["b"][0].is_null());
  EXPECT_TRUE((*j)["b"][1].is_null());
  EXPECT_EQ((*j)["c"], "NaN");
}

TEST(LegacyJson, NonFiniteTokensAreReportedByPointer) {
  std::vector<NonFinite> seen;
  auto j = parse_legacy(R"j({"Ar40": {"value": NaN, "error": 1.5}, "L2(CDD)": [1, -Infinity], "x": Infinity})j", &seen);
  ASSERT_TRUE(j.has_value()) << j.error().what;
  ASSERT_EQ(seen.size(), 3u);
  EXPECT_EQ(seen[0].pointer, "/Ar40/value");
  EXPECT_EQ(seen[0].token, "NaN");
  EXPECT_EQ(seen[1].pointer, "/L2(CDD)/1");
  EXPECT_EQ(seen[1].token, "-Infinity");
  EXPECT_EQ(seen[2].pointer, "/x");
  EXPECT_EQ(seen[2].token, "Infinity");
  EXPECT_TRUE((*j)["Ar40"]["value"].is_null());
  EXPECT_EQ((*j)["Ar40"]["error"], 1.5);

  const Json under = nonfinite_under(seen, pointer_of("Ar40"));
  EXPECT_EQ(dump(under), R"({"/value":"NaN"})");
  EXPECT_TRUE(nonfinite_under(seen, pointer_of("Ar39")).is_null());
}

TEST(LegacyJson, TokensInsideStringsAreUntouched) {
  // Letters, an escaped quote and a backslash before the closing quote.
  auto j = parse_legacy(R"({"NaN": "x Infinity \" NaN", "p": "-Infinity\\", "q": NaN})");
  ASSERT_TRUE(j.has_value()) << j.error().what;
  EXPECT_EQ((*j)["NaN"], "x Infinity \" NaN");
  EXPECT_EQ((*j)["p"], "-Infinity\\");
  EXPECT_TRUE((*j)["q"].is_null());
}

TEST(LegacyJson, AWordThatOnlyStartsLikeATokenIsAnError) {
  EXPECT_FALSE(parse_legacy(R"({"a": NaNx})").has_value());
  EXPECT_FALSE(parse_legacy(R"({"a": Infinityy})").has_value());
  EXPECT_FALSE(parse_legacy(R"({"a": nan})").has_value());
}

TEST(LegacyJson, BomAndCrlf) {
  auto j = parse_legacy("\xEF\xBB\xBF{\r\n  \"a\": 1,\r\n  \"b\": \"x\"\r\n}\r\n");
  ASSERT_TRUE(j.has_value()) << j.error().what;
  EXPECT_EQ((*j)["a"], 1);
  EXPECT_EQ((*j)["b"], "x");
}

TEST(LegacyJson, TruncatedIsError) {
  auto j = parse_legacy(R"({"a": 1, "b": [1, 2)");
  ASSERT_FALSE(j.has_value());
  EXPECT_EQ(j.error().kind, ErrorKind::Protocol);
  EXPECT_FALSE(parse_legacy("").has_value());
  EXPECT_FALSE(parse_legacy("\xEF\xBB\xBF").has_value());
  EXPECT_FALSE(parse_legacy(R"({"a": "unterminated)").has_value());
  EXPECT_FALSE(parse_legacy("{} trailing").has_value());
}

TEST(LegacyJson, Conversions) {
  EXPECT_EQ(as_double(Json(2.5)), 2.5);
  EXPECT_EQ(as_double(Json(3)), 3.0);
  EXPECT_EQ(as_double(Json("3.0")), 3.0);
  EXPECT_EQ(as_double(Json(" 4e-2 ")), 0.04);
  EXPECT_FALSE(as_double(Json("")).has_value());
  EXPECT_FALSE(as_double(Json("3.0 mm")).has_value());
  EXPECT_FALSE(as_double(Json("nan")).has_value());
  EXPECT_FALSE(as_double(Json(true)).has_value());
  EXPECT_FALSE(as_double(Json(nullptr)).has_value());

  EXPECT_EQ(as_int(Json(4)), 4);
  EXPECT_EQ(as_int(Json("25")), 25);
  EXPECT_EQ(as_int(Json(16.0)), 16);
  EXPECT_FALSE(as_int(Json(16.5)).has_value());
  EXPECT_FALSE(as_int(Json("")).has_value());
  EXPECT_FALSE(as_int(Json("A3")).has_value());
  EXPECT_FALSE(as_int(Json(1e12)).has_value());
  EXPECT_FALSE(as_int(Json(-7399522718437156748LL)).has_value());

  EXPECT_EQ(as_bool(Json(true)), true);
  EXPECT_EQ(as_bool(Json(0)), false);
  EXPECT_EQ(as_bool(Json("True")), true);
  EXPECT_FALSE(as_bool(Json("ok")).has_value());
  EXPECT_FALSE(as_bool(Json(2)).has_value());

  EXPECT_EQ(as_text(Json("x")), "x");
  EXPECT_EQ(as_text(Json(66052)), "66052");
  EXPECT_FALSE(as_text(Json::array()).has_value());
}

TEST(LegacyJson, TakeErasesWhatItRepresents) {
  Json o = {{"n", "25"}, {"bad", "A3"}, {"empty", ""}, {"null", nullptr}, {"t", "x"}, {"j", {{"k", 1}}}};
  EXPECT_EQ(take_int(o, "n"), 25);
  EXPECT_FALSE(take_int(o, "bad").has_value());
  EXPECT_FALSE(take_double(o, "empty").has_value());
  EXPECT_FALSE(take_text(o, "null").has_value());
  EXPECT_FALSE(take_text(o, "absent").has_value());
  EXPECT_EQ(take_text(o, "t"), "x");
  EXPECT_EQ(take_json(o, "j"), R"({"k":1})");
  // Only the value that could not be converted is left for extra.
  EXPECT_EQ(dump(o), R"({"bad":"A3"})");
  EXPECT_EQ(extra_text(o), R"({"bad":"A3"})");
  EXPECT_FALSE(extra_text(Json::object()).has_value());
}

}  // namespace
}  // namespace pychron::dvc
