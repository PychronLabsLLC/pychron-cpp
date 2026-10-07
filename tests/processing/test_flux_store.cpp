// Flux monitor sets (flux fitting design, section 4): the defaults, lambda_k,
// the revisioned document (round trip, unknown keys, conflict) and the
// validation of a document.

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <variant>
#include <vector>
#include <variant>
#include <vector>

#include "flux_store_fixture.hpp"
#include "pychron/processing/flux_store.hpp"

namespace pychron::processing {
namespace {

namespace ps = pychron::persistence;

class FluxMonitors : public testing::FluxStoreTest {};

TEST_F(FluxMonitors, AStoreWithNoDocumentHasTheTwoDefaults) {
  auto loaded = load_monitor_sets(store());
  ASSERT_TRUE(loaded) << to_string(loaded.error());
  EXPECT_FALSE(loaded->ref_object);
  const auto& sets = loaded->sets;
  ASSERT_EQ(sets.sets.size(), 2u);
  EXPECT_EQ(sets.default_name, "FC-2 (Kuiper 2008)");
  const auto& k = sets.sets[0];
  EXPECT_EQ(k.name, "FC-2 (Kuiper 2008)");
  EXPECT_EQ(k.sample, "FC-2");
  EXPECT_EQ(k.material, "sanidine");
  EXPECT_DOUBLE_EQ(k.age_ma, 28.201);
  EXPECT_DOUBLE_EQ(k.age_err_ma, 0.046);
  EXPECT_DOUBLE_EQ(k.lambda_ec.value, 5.80e-11);
  EXPECT_DOUBLE_EQ(k.lambda_ec.error, 9.9e-13);
  EXPECT_DOUBLE_EQ(k.lambda_b.value, 4.883e-10);
  EXPECT_DOUBLE_EQ(k.lambda_b.error, 1.4e-12);
  const auto& r = sets.sets[1];
  EXPECT_EQ(r.name, "FC-2 (Renne 1998)");
  EXPECT_EQ(r.sample, "FC-2");
  EXPECT_EQ(r.material, "sanidine");
  EXPECT_DOUBLE_EQ(r.age_ma, 28.02);
  EXPECT_DOUBLE_EQ(r.age_err_ma, 0.16);
  EXPECT_DOUBLE_EQ(r.lambda_ec.value, 5.81e-11);
  EXPECT_DOUBLE_EQ(r.lambda_ec.error, 0.0);
  EXPECT_DOUBLE_EQ(r.lambda_b.value, 4.962e-10);
  EXPECT_DOUBLE_EQ(r.lambda_b.error, 0.0);
  ASSERT_NE(sets.find(""), nullptr);
  EXPECT_EQ(sets.find("")->name, "FC-2 (Kuiper 2008)");
  EXPECT_EQ(sets.find("FC-2 (Renne 1998)"), &sets.sets[1]);
  EXPECT_EQ(sets.find("nope"), nullptr);
}

TEST_F(FluxMonitors, LambdaKIsTheSumWithErrorsInQuadrature) {
  const MonitorSets defaults = default_monitor_sets();
  const MonitorSet& k = defaults.sets[0];
  const auto lk = k.lambda_k();
  EXPECT_NEAR(lk.value, 5.463e-10, 1e-22);
  EXPECT_DOUBLE_EQ(lk.error, std::sqrt(9.9e-13 * 9.9e-13 + 1.4e-12 * 1.4e-12));
  EXPECT_DOUBLE_EQ(k.constants().age_a, 28.201e6);
  EXPECT_DOUBLE_EQ(k.constants().lambda_k, lk.value);
}

TEST_F(FluxMonitors, SaveThenLoadRoundTripsAndKeepsUnknownKeys) {
  auto parsed = parse_monitor_sets(R"({"default":"B","lab":"NMGRL","monitors":[
    {"name":"A","sample":"FC-2","material":"sanidine","age_ma":28.2,"age_err_ma":0.1,
     "lambda_ec":[5.8e-11,1e-12],"lambda_b":[4.9e-10,2e-12]},
    {"name":"B","sample":"Hb3gr","material":"hornblende","age_ma":1080,"age_err_ma":1,
     "lambda_ec":[5.8e-11,0],"lambda_b":[4.9e-10,0]}]})");
  ASSERT_TRUE(parsed) << to_string(parsed.error());
  auto before = load_monitor_sets(store());
  ASSERT_TRUE(before);
  auto outcome = save_monitor_sets(store(), actor(), *parsed, *before);
  ASSERT_TRUE(outcome) << to_string(outcome.error());
  ASSERT_TRUE(std::holds_alternative<ps::Committed>(*outcome));
  auto after = load_monitor_sets(store());
  ASSERT_TRUE(after) << to_string(after.error());
  EXPECT_TRUE(after->ref_object);
  EXPECT_TRUE(after->head);
  EXPECT_EQ(after->sets, *parsed);
  EXPECT_EQ(after->sets.default_name, "B");
  EXPECT_NE(after->sets.other_json.find("NMGRL"), std::string::npos);
  EXPECT_NE(to_json(after->sets).find("\"lab\""), std::string::npos);
}

TEST_F(FluxMonitors, ASaveOnAStaleHeadIsAConflict) {
  auto first = load_monitor_sets(store());
  ASSERT_TRUE(first);
  ASSERT_TRUE(save_monitor_sets(store(), actor(), first->sets, *first));
  auto a = load_monitor_sets(store());
  auto b = load_monitor_sets(store());
  ASSERT_TRUE(a && b);
  MonitorSets changed = a->sets;
  changed.sets[0].age_ma = 28.3;
  auto one = save_monitor_sets(store(), actor(), changed, *a);
  ASSERT_TRUE(one);
  EXPECT_TRUE(std::holds_alternative<ps::Committed>(*one));
  changed.sets[0].age_ma = 28.4;
  auto two = save_monitor_sets(store(), actor(), changed, *b);
  ASSERT_TRUE(two) << to_string(two.error());
  EXPECT_TRUE(std::holds_alternative<std::vector<ps::Conflict>>(*two));
}

TEST(FluxMonitorsParse, RejectsWhatIsNotValid) {
  const auto set = [](const std::string& name = "A", const std::string& age = "28", const std::string& age_err = "0.1",
                      const std::string& ec = "[5.8e-11,1e-12]", const std::string& b = "[4.9e-10,2e-12]") {
    return R"({"name":")" + name + R"(","sample":"FC-2","material":"sanidine","age_ma":)" + age +
           R"(,"age_err_ma":)" + age_err + R"(,"lambda_ec":)" + ec + R"(,"lambda_b":)" + b + "}";
  };
  const auto doc = [](const std::string& monitors, const std::string& def = "A") {
    return R"({"default":")" + def + R"(","monitors":)" + monitors + "}";
  };
  struct Case {
    std::string json, names;
  };
  const Case cases[] = {
      {"[1,2]", "object"},
      {doc("{}"), "monitors"},
      {doc("[]"), "monitors"},
      {doc("[{\"sample\":\"FC-2\"}]"), "name"},
      {doc("[" + set("A") + "," + set("A") + "]"), "name"},
      {doc("[" + set("A") + "]", "Z"), "default"},
      {doc("[" + set("A", "0") + "]"), "age_ma"},
      {doc("[" + set("A", "-1") + "]"), "age_ma"},
      {doc("[" + set("A", "28", "-0.1") + "]"), "age_err_ma"},
      {doc("[" + set("A", "28", "0.1", "[5.8e-11,1e-12]", "[4.9e-10]") + "]"), "lambda_b"},
      {doc("[" + set("A", "28", "0.1", "[5.8e-11,1e-12]", "4.9e-10") + "]"), "lambda_b"},
      {doc("[" + set("A", "28", "0.1", "[5.8e-11,-1e-12]") + "]"), "lambda_ec"},
      {doc("[" + set("A", "28", "0.1", "[5.8e-11,1e-12]", "[4.9e-10,-2e-12]") + "]"), "lambda_b"},
  };
  for (const auto& c : cases) {
    auto r = parse_monitor_sets(c.json);
    ASSERT_FALSE(r) << c.json;
    EXPECT_NE(r.error().what.find(c.names), std::string::npos) << c.json << " -> " << r.error().what;
  }
  EXPECT_TRUE(parse_monitor_sets(doc("[" + set("A") + "]")));
}

}  // namespace
}  // namespace pychron::processing
