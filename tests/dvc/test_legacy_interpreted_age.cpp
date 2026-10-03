// parse_interpreted_age on one real file of each format (README 5.9), and
// parse_frozen_production. Literals were copied by hand from the fixtures.

#include <gtest/gtest.h>

#include <map>
#include <string>

#include <nlohmann/json.hpp>

#include "fixture_files.hpp"
#include "pychron/dvc/legacy_layout.hpp"

namespace pychron::dvc {
namespace {

namespace ps = pychron::persistence;
using nlohmann::json;
using testing::fixture;

TEST(Layout, InterpretedAgeFixture) {
  // The 2018 flat format.
  const std::string text = fixture("ia/IR1010/660/ia/52.ia.json");
  auto ia = parse_interpreted_age(text, classify_path("660/ia/52.ia.json").key);
  ASSERT_TRUE(ia.has_value()) << ia.error().what;
  EXPECT_FALSE(ia->nested);
  EXPECT_EQ(ia->name, "01");
  EXPECT_EQ(ia->uuid.value().str(), "d9cb9f9a-250e-4bd3-8c25-a7186c51a94a");
  EXPECT_EQ(ia->identifier, "66052");
  // The top-level age and age_err are 0.0 in this file; the preferred age is
  // the "age" entry of preferred_kinds.
  EXPECT_EQ(ia->value.age, 20.587952457197492);
  EXPECT_EQ(ia->value.age_err, 0.9477837369275852);
  EXPECT_EQ(ia->value.age_kind, "Plateau");
  EXPECT_EQ(ia->value.kca, 0.04168595299010074);
  EXPECT_EQ(ia->value.kca_err, 0.00031601188696557623);
  EXPECT_EQ(ia->value.mswd, 0.0);
  EXPECT_EQ(ia->value.nanalyses, 0);
  EXPECT_EQ(json::parse(ia->value.doc_json), json::parse(text));  // the whole file

  ASSERT_EQ(ia->members.size(), 13u);
  EXPECT_EQ(ia->members[0].analysis.value().str(), "42c7c7e3-5a6c-43b9-9236-b25ce75780fa");
  EXPECT_EQ(ia->members[0].record_id, "66052-01A");
  EXPECT_EQ(ia->members[0].age, 20.618793060387148);
  EXPECT_EQ(ia->members[0].age_err, 1.7370644476853319);
  // README 5.9: the fixture unknown is analyses[4].
  EXPECT_EQ(ia->members[4].analysis.value().str(), "15fb3686-4aed-40c1-8987-e73a8a52b434");
  EXPECT_EQ(ia->members[4].record_id, "66052-01E");
  EXPECT_EQ(ia->members[4].age, 24.03351976363802);
  EXPECT_EQ(ia->members[4].age_err, 0.9085947707302583);

  ASSERT_EQ(ia->value.members.size(), 13u);
  EXPECT_EQ(ia->value.members[0].analysis.str(), "42c7c7e3-5a6c-43b9-9236-b25ce75780fa");
  EXPECT_EQ(ia->value.members[0].record_id, "66052-01A");
  EXPECT_EQ(ia->value.members[0].plateau_step, false);
  EXPECT_EQ(ia->value.members[0].tag, "ok");
  EXPECT_EQ(ia->value.members[4].tag, "omit");
  EXPECT_TRUE(ia->notes.empty());
}

TEST(Layout, InterpretedAgeNestedFixture) {
  // The later format: no identifier key, everything under "preferred".
  const std::string text = fixture("ia/Cornudas/694/ia/21_00000.ia.json");
  auto ia = parse_interpreted_age(text, classify_path("694/ia/21_00000.ia.json").key);
  ASSERT_TRUE(ia.has_value()) << ia.error().what;
  EXPECT_TRUE(ia->nested);
  EXPECT_EQ(ia->name, "02");
  EXPECT_EQ(ia->uuid.value().str(), "207655ea-1a0f-43fb-b8f2-fd6fac19db66");
  EXPECT_EQ(ia->identifier, "69421");  // from the path: 694 + 21, counter removed
  EXPECT_EQ(ia->value.age, 31.918606680860304);
  EXPECT_EQ(ia->value.age_err, 0.029193285068718947);
  EXPECT_EQ(ia->value.age_kind, "Plateau");
  EXPECT_EQ(ia->value.kca, 19.16875065501418);
  EXPECT_EQ(ia->value.kca_err, 0.38535034017024394);
  EXPECT_EQ(ia->value.mswd, 2.5600268206970083);
  EXPECT_EQ(ia->value.nanalyses, 5);
  EXPECT_EQ(json::parse(ia->value.doc_json), json::parse(text));

  ASSERT_EQ(ia->members.size(), 10u);
  EXPECT_EQ(ia->members[0].analysis.value().str(), "9f1bf0da-f493-4fba-84dd-de41145a538b");
  EXPECT_EQ(ia->members[0].record_id, "69421-02A");
  EXPECT_EQ(ia->members[0].age, 10.256790546470379);
  EXPECT_EQ(ia->members[0].age_err, 1.8277506553563656);
  EXPECT_EQ(ia->members[9].analysis.value().str(), "015aae9e-263d-4261-a34b-1b4bcc83d2fa");
  EXPECT_EQ(ia->members[9].record_id, "69421-02J");
  EXPECT_EQ(ia->members[9].age, 31.8865006223828);
  EXPECT_EQ(ia->members[9].age_err, 0.061794942019893044);
  ASSERT_EQ(ia->value.members.size(), 10u);
  EXPECT_EQ(ia->value.members[0].plateau_step, false);
  EXPECT_EQ(ia->value.members[9].plateau_step, true);
  EXPECT_EQ(ia->value.members[9].tag, "ok");
}

TEST(Layout, InterpretedAgeIdentifierFromThePathOrTheMembers) {
  const std::string nested = R"({"name": "a", "uuid": "207655ea-1a0f-43fb-b8f2-fd6fac19db66",
      "preferred": {"age": 1.5, "age_err": 0.5}, "sample_metadata": {},
      "analyses": [{"uuid": "9f1bf0da-f493-4fba-84dd-de41145a538b", "record_id": "69421-02A", "age": 1.0,
                    "age_err": 0.1}]})";
  // Early 2018 name (no counter), later name (counter), and no path at all.
  EXPECT_EQ(parse_interpreted_age(nested, "69421")->identifier, "69421");
  EXPECT_EQ(parse_interpreted_age(nested, "69421_00012")->identifier, "69421");
  EXPECT_EQ(parse_interpreted_age(nested, "")->identifier, "");
  // Without preferred_kinds the plain age is used and there is no kind.
  auto ia = parse_interpreted_age(nested, "69421");
  EXPECT_EQ(ia->value.age, 1.5);
  EXPECT_EQ(ia->value.age_err, 0.5);
  EXPECT_FALSE(ia->value.age_kind.has_value());
  EXPECT_FALSE(ia->value.kca.has_value());
  // The file's own identifier wins over the path.
  auto flat = parse_interpreted_age(R"({"name": "a", "identifier": "66052", "age": 2.5, "age_err": 0.25,
                                        "analyses": []})",
                                    "99999_00000");
  ASSERT_TRUE(flat.has_value()) << flat.error().what;
  EXPECT_EQ(flat->identifier, "66052");
  EXPECT_FALSE(flat->nested);
  EXPECT_EQ(flat->value.age, 2.5);
  EXPECT_FALSE(flat->uuid.has_value());
}

TEST(Layout, InterpretedAgeMembersWithoutUuidOrAge) {
  auto ia = parse_interpreted_age(
      R"({"name": "a", "identifier": "66052", "age": NaN, "age_err": 0.25,
          "analyses": [{"record_id": "66052-01A", "age": 1.0, "age_err": 0.1},
                       {"uuid": "nope", "record_id": "66052-01B", "age": NaN, "age_err": Infinity},
                       {"uuid": "15fb3686-4aed-40c1-8987-e73a8a52b434", "record_id": "66052-01E", "age_err": null,
                        "plateau_step": true, "tag": "omit"}]})");
  ASSERT_TRUE(ia.has_value()) << ia.error().what;
  // A NaN preferred age is unknown, never 0.
  EXPECT_FALSE(ia->value.age.has_value());
  EXPECT_EQ(ia->value.age_err, 0.25);
  // Every analysis is reported for verify, in file order.
  ASSERT_EQ(ia->members.size(), 3u);
  EXPECT_FALSE(ia->members[0].analysis.has_value());
  EXPECT_EQ(ia->members[0].age, 1.0);
  EXPECT_FALSE(ia->members[1].analysis.has_value());
  EXPECT_FALSE(ia->members[1].age.has_value());
  EXPECT_FALSE(ia->members[1].age_err.has_value());
  EXPECT_EQ(ia->members[2].analysis.value().str(), "15fb3686-4aed-40c1-8987-e73a8a52b434");
  EXPECT_FALSE(ia->members[2].age.has_value());
  EXPECT_FALSE(ia->members[2].age_err.has_value());
  // Only a member with a uuid can be a member row; the others are noted.
  ASSERT_EQ(ia->value.members.size(), 1u);
  EXPECT_EQ(ia->value.members[0].record_id, "66052-01E");
  EXPECT_EQ(ia->value.members[0].plateau_step, true);
  EXPECT_EQ(ia->value.members[0].tag, "omit");
  ASSERT_EQ(ia->notes.size(), 3u);
  EXPECT_NE(ia->notes[0].find("66052-01A"), std::string::npos);
  EXPECT_NE(ia->notes[1].find("66052-01B"), std::string::npos);
  EXPECT_NE(ia->notes[2].find("NaN"), std::string::npos);  // the tokens that became null
  // doc_json is valid JSON with the tokens as null.
  EXPECT_TRUE(json::parse(ia->value.doc_json)["age"].is_null());
}

TEST(Layout, InterpretedAgeErrors) {
  EXPECT_FALSE(parse_interpreted_age("not json").has_value());
  EXPECT_FALSE(parse_interpreted_age("[]").has_value());
  EXPECT_FALSE(parse_interpreted_age(R"({"age": 1.0, "analyses": []})").has_value());        // no name
  EXPECT_FALSE(parse_interpreted_age(R"({"name": "a", "analyses": {"a": 1}})").has_value());  // analyses not a list
  EXPECT_FALSE(parse_interpreted_age(R"({"name": "a", "analyses": [1]})").has_value());
  // No analyses key at all is an interpreted age with no members.
  auto none = parse_interpreted_age(R"({"name": "a", "age": 1.0})");
  ASSERT_TRUE(none.has_value()) << none.error().what;
  EXPECT_TRUE(none->members.empty());
}

// ---------------------------------------------------------------- frozen production

TEST(Layout, FrozenProduction) {
  // No frozen production file was available; the production file of the meta
  // repo has the same shape (README 6.2).
  auto p = parse_frozen_production(fixture("meta/NM-293/productions/Triga_PR.json"), "NM-293", "G");
  ASSERT_TRUE(p.has_value()) << p.error().what;
  EXPECT_EQ(p->irradiation, "NM-293");
  EXPECT_EQ(p->level, "G");
  EXPECT_FALSE(p->name.has_value());
  EXPECT_FALSE(p->value.reactor.has_value());
  EXPECT_FALSE(p->extra_json.has_value());
  ASSERT_EQ(p->value.ratios.size(), 9u);
  std::map<std::string, ps::ProductionRatio> by;
  for (const auto& r : p->value.ratios) by[r.key] = r;
  EXPECT_EQ(by.at("Ca3637").value, 0.000286);
  EXPECT_EQ(by.at("Ca3637").error, 5e-07);
  EXPECT_EQ(by.at("K4039").value, 0.00873);
  EXPECT_EQ(by.at("K4039").error, 0.00017);
  EXPECT_EQ(by.at("Cl3638").value, 250.0);
  EXPECT_EQ(by.at("K3739").value, 0.0);
  EXPECT_TRUE(ps::ref_payload_matches(ps::RefType::Production, ps::RefPayload{p->value}));
}

TEST(Layout, FrozenProductionOptionalKeysAndErrors) {
  auto p = parse_frozen_production(R"({"Ca3637": [0.000286, 5e-07], "K4039": 0.00873, "reactor": "Triga",
                                       "name": "Triga_PR", "note": "frozen", "zz_new": {"a": 1}})",
                                   "NM-312", "F");
  ASSERT_TRUE(p.has_value()) << p.error().what;
  EXPECT_EQ(p->value.reactor, "Triga");
  EXPECT_EQ(p->value.note, "frozen");
  EXPECT_EQ(p->name, "Triga_PR");
  ASSERT_EQ(p->value.ratios.size(), 2u);
  EXPECT_EQ(p->value.ratios[1].key, "K4039");
  EXPECT_EQ(p->value.ratios[1].value, 0.00873);
  EXPECT_EQ(p->value.ratios[1].error, 0.0);  // a bare number has no error
  EXPECT_EQ(json::parse(p->extra_json.value()), json::parse(R"({"zz_new": {"a": 1}})"));

  // ProductionRatio holds plain doubles: a ratio that is not a finite number
  // cannot be stored as one and is refused rather than written as 0.
  EXPECT_FALSE(parse_frozen_production(R"({"Ca3637": [NaN, 5e-07]})", "NM-312", "F").has_value());
  EXPECT_FALSE(parse_frozen_production(R"({"Ca3637": [0.1, Infinity]})", "NM-312", "F").has_value());
  EXPECT_FALSE(parse_frozen_production(R"({"Ca3637": [0.1]})", "NM-312", "F").has_value());
  EXPECT_FALSE(parse_frozen_production("not json", "NM-312", "F").has_value());
  EXPECT_FALSE(parse_frozen_production("[]", "NM-312", "F").has_value());
}

}  // namespace
}  // namespace pychron::dvc
