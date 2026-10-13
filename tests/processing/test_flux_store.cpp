// Flux monitor sets (flux fitting design, section 4): the defaults, lambda_k,
// the revisioned document (round trip, unknown keys, conflict) and the
// validation of a document. The options JSON of a saved fit (6.4),
// load_level (6.1) and save_level (6.3) over the seeded level of flux_seed.hpp.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "../reduction/flux_golden.hpp"
#include "flux_store_fixture.hpp"
#include "pychron/processing/flux_store.hpp"
#include "pychron/processing/reduced.hpp"

namespace pychron::processing {
namespace {

namespace ps = pychron::persistence;

class FluxMonitors : public testing::FluxStoreTest {};

TEST_P(FluxMonitors, AStoreWithNoDocumentHasTheTwoDefaults) {
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

TEST_P(FluxMonitors, LambdaKIsTheSumWithErrorsInQuadrature) {
  const MonitorSets defaults = default_monitor_sets();
  const MonitorSet& k = defaults.sets[0];
  const auto lk = k.lambda_k();
  EXPECT_NEAR(lk.value, 5.463e-10, 1e-22);
  EXPECT_DOUBLE_EQ(lk.error, std::sqrt(9.9e-13 * 9.9e-13 + 1.4e-12 * 1.4e-12));
  EXPECT_DOUBLE_EQ(k.constants().age_a, 28.201e6);
  EXPECT_DOUBLE_EQ(k.constants().lambda_k, lk.value);
}

TEST_P(FluxMonitors, SaveThenLoadRoundTripsAndKeepsUnknownKeys) {
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

TEST_P(FluxMonitors, ASaveOnAStaleHeadIsAConflict) {
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
      {doc(R"([{"sample":"FC-2"}])"), "name"},
      {doc("[" + set("A") + "," + set("A") + "]"), "name"},
      {doc("[" + set("A") + "]", "Z"), "default"},
      {doc(R"([{"name":"A","age_ma":28,"age_err_ma":0.1,"lambda_ec":[5.8e-11,0],"lambda_b":[4.9e-10,0]}])"), "sample"},
      {doc("[" + set("A", "28").replace(set("A", "28").find("FC-2"), 4, "") + "]"), "sample"},
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
  // Errors in a set name the key and the set.
  const std::string no_sample =
      R"j({"monitors":[{"name":"FC-2 (X)","age_ma":28,"age_err_ma":0.1,"lambda_ec":[5.8e-11,0],"lambda_b":[4.9e-10,0]}]})j";
  auto r = parse_monitor_sets(no_sample);
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().what.find("'sample'"), std::string::npos) << r.error().what;
  EXPECT_NE(r.error().what.find("FC-2 (X)"), std::string::npos) << r.error().what;
  EXPECT_TRUE(parse_monitor_sets(doc("[" + set("A") + "]")));
}

TEST(FluxMonitorsParse, ADocumentWithNoDefaultTakesTheFirstSet) {
  auto r = parse_monitor_sets(R"({"monitors":[
    {"name":"A","sample":"FC-2","age_ma":28,"age_err_ma":0.1,"lambda_ec":[5.8e-11,0],"lambda_b":[4.9e-10,0]},
    {"name":"B","sample":"FC-2","age_ma":28,"age_err_ma":0.1,"lambda_ec":[5.8e-11,0],"lambda_b":[4.9e-10,0]}]})");
  ASSERT_TRUE(r) << to_string(r.error());
  EXPECT_EQ(r->default_name, "A");
}

// ---- The options JSON (design section 6.4) ----------------------------------

namespace pr = pychron::reduction;

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// Options as stored: PostgreSQL's jsonb gives them back with its own key
// order and spacing, so they are compared as JSON.
nlohmann::json json_of(const std::optional<std::string>& text) {
  return nlohmann::json::parse(text.value_or("null"), nullptr, false);
}

TEST(FluxOptionsJson, RoundTrip) {
  FluxOptions o;
  o.fit.kind = pr::ModelKind::NearestNeighbors;
  o.fit.weighted = true;
  o.fit.error = pr::MeanErrorKind::Sd;
  o.fit.n_neighbors = 3;
  o.fit.interpolation = pr::Interpolation::Linear;
  o.fit.axis = pr::Axis::Y;
  o.fit.degree = 3;
  o.mean = pr::MeanKind::Weighted;
  o.mean_error = pr::MeanErrorKind::Sem;
  const MonitorSet renne = default_monitor_sets().sets[1];
  const std::string json = flux_options_json(o, renne, false, true, true, 1.12, 5, "pychron-cpp 0.4.0");
  for (const char* member :
       {R"j("model_kind":"Nearest Neighbors")j", R"j("use_weighted_fit":true)j", R"j("predicted_j_error_type":"sd")j",
        R"j("error_kind":"sem")j", R"j("mean_kind":"weighted")j", R"j("n_neighbors":3)j", R"j("interpolation_style":"Linear")j",
        R"j("one_d_axis":"Y")j", R"j("degree":3)j", R"j("monitor_reference":"FC-2 (Renne 1998)")j",
        R"j("monitor_sample":"FC-2")j", R"j("used_in_fit":false)j", R"j("excluded":true)j", R"j("all_positions":true)j",
        R"j("fit_mswd":1.12)j", R"j("fit_dof":5)j",
        R"j("software":"pychron-cpp 0.4.0")j"})
    EXPECT_TRUE(has(json, member)) << member << " in " << json;

  const FluxOptionsDoc doc = parse_flux_options(json);
  ASSERT_TRUE(doc.options);
  EXPECT_EQ(*doc.options, o);
  EXPECT_EQ(doc.monitor_set, "FC-2 (Renne 1998)");
  EXPECT_EQ(doc.monitor_sample, "FC-2");
  ASSERT_TRUE(doc.used_in_fit.has_value());
  EXPECT_FALSE(*doc.used_in_fit);
  EXPECT_EQ(doc.excluded, std::optional<bool>(true));
  EXPECT_EQ(doc.all_positions, std::optional<bool>(true));
  EXPECT_FALSE(doc.sd_replaced);

  // Every model, interpolation, axis and error kind comes back as written.
  for (auto kind : {pr::ModelKind::Plane, pr::ModelKind::Bowl, pr::ModelKind::WeightedMean, pr::ModelKind::Matching,
                    pr::ModelKind::NearestNeighbors, pr::ModelKind::Bracketing, pr::ModelKind::LeastSquares1D,
                    pr::ModelKind::WeightedMean1D, pr::ModelKind::Bracketing1D})
    for (auto interpolation : {pr::Interpolation::WeightedMean, pr::Interpolation::Average, pr::Interpolation::Linear})
      for (auto axis : {pr::Axis::X, pr::Axis::Y})
        for (auto error : {pr::MeanErrorKind::Sem, pr::MeanErrorKind::Msem}) {
          FluxOptions each;
          each.fit.kind = kind;
          each.fit.interpolation = interpolation;
          each.fit.axis = axis;
          each.fit.error = error;
          each.mean_error = pr::MeanErrorKind::Sd;
          const FluxOptionsDoc back = parse_flux_options(flux_options_json(each, renne, true, false, false, 0.5, 2, "t"));
          ASSERT_TRUE(back.options) << legacy_model_name(kind);
          EXPECT_EQ(*back.options, each) << legacy_model_name(kind);
          EXPECT_EQ(back.used_in_fit, std::optional<bool>(true));
          EXPECT_EQ(back.excluded, std::optional<bool>(false));
          EXPECT_EQ(back.all_positions, std::optional<bool>(false));
          EXPECT_FALSE(back.sd_replaced);
        }
}

TEST(FluxOptionsJson, ReadsALegacyDict) {
  const FluxOptionsDoc doc = parse_flux_options(
      R"({"model_kind":"Bowl","use_weighted_fit":true,"predicted_j_error_type":"SEM",
          "interpolation_style":"Linear","monitor_reference":"FC Min"})");
  ASSERT_TRUE(doc.options);
  FluxOptions expected;  // what the dict does not say is the default
  expected.fit.kind = pr::ModelKind::Bowl;
  expected.fit.weighted = true;
  expected.fit.error = pr::MeanErrorKind::Sem;
  expected.fit.interpolation = pr::Interpolation::Linear;
  EXPECT_EQ(*doc.options, expected);
  EXPECT_EQ(doc.monitor_set, "FC Min");
  EXPECT_EQ(doc.monitor_sample, "");
  EXPECT_FALSE(doc.used_in_fit.has_value());
  EXPECT_FALSE(doc.excluded.has_value());  // keys of this version only
  EXPECT_FALSE(doc.all_positions.has_value());
  EXPECT_FALSE(doc.sd_replaced);
}

TEST(FluxOptionsJson, LegacyMsemString) {
  for (const char* text : {"SE but if MSWD>1 use SE * sqrt(MSWD)", "SEM, but if MSWD>1 use SEM * sqrt(MSWD)"}) {
    const FluxOptionsDoc doc = parse_flux_options(
        std::string(R"({"model_kind":"Weighted Mean","predicted_j_error_type":")") + text + R"(","error_kind":")" + text +
        "\"}");
    ASSERT_TRUE(doc.options) << text;
    EXPECT_EQ(doc.options->fit.kind, pr::ModelKind::WeightedMean);
    EXPECT_EQ(doc.options->fit.error, pr::MeanErrorKind::Msem) << text;
    EXPECT_EQ(doc.options->mean_error, pr::MeanErrorKind::Msem) << text;
  }
  // The other legacy spellings, so Msem above is not just the default.
  const FluxOptionsDoc sem =
      parse_flux_options(R"({"model_kind":"Weighted Mean","predicted_j_error_type":"SEM","error_kind":"SD"})");
  ASSERT_TRUE(sem.options);
  EXPECT_EQ(sem.options->fit.error, pr::MeanErrorKind::Sem);
  EXPECT_EQ(sem.options->mean_error, pr::MeanErrorKind::Sd);
  EXPECT_FALSE(sem.sd_replaced);
}

TEST(FluxOptionsJson, UnknownModelOrGarbageMeansNoOptions) {
  for (const char* text : {R"({"model_kind":"RBF","monitor_reference":"FC Min","used_in_fit":false})",
                           R"({"model_kind":""})", "", "{}", "not json", "[]", R"({"model_kind":7})", "null", "3"}) {
    const FluxOptionsDoc doc = parse_flux_options(text);
    EXPECT_FALSE(doc.options) << text;
    EXPECT_FALSE(doc.sd_replaced) << text;
  }
  // What else an unknown model's dict says is still read.
  const FluxOptionsDoc rbf = parse_flux_options(R"({"model_kind":"RBF","monitor_reference":"FC Min","used_in_fit":false})");
  EXPECT_EQ(rbf.monitor_set, "FC Min");
  EXPECT_EQ(rbf.used_in_fit, std::optional<bool>(false));
  // A mistyped member takes its default.
  const FluxOptionsDoc odd = parse_flux_options(
      R"({"model_kind":"plane","use_weighted_fit":"yes","n_neighbors":"3","degree":2.5,"one_d_axis":4,
          "predicted_j_error_type":"nonsense","monitor_reference":12,"used_in_fit":"no","excluded":1,"all_positions":"yes"})");
  ASSERT_TRUE(odd.options);
  EXPECT_EQ(*odd.options, FluxOptions{});
  EXPECT_EQ(odd.monitor_set, "");
  EXPECT_FALSE(odd.used_in_fit.has_value());
  EXPECT_FALSE(odd.excluded.has_value());
  EXPECT_FALSE(odd.all_positions.has_value());
}

TEST(FluxOptionsJson, SdOnASurfaceReadsAsMsem) {
  for (const char* model : {"Plane", "Bowl", "LeastSquares1D"}) {
    const FluxOptionsDoc doc = parse_flux_options(std::string(R"({"model_kind":")") + model +
                                                  R"(","predicted_j_error_type":"SD","error_kind":"SD"})");
    ASSERT_TRUE(doc.options) << model;
    EXPECT_EQ(doc.options->fit.error, pr::MeanErrorKind::Msem) << model;
    EXPECT_EQ(doc.options->mean_error, pr::MeanErrorKind::Sd) << model;  // a position's mean may use SD
    EXPECT_TRUE(doc.sd_replaced) << model;
  }
  const FluxOptionsDoc mean = parse_flux_options(R"({"model_kind":"Weighted Mean","predicted_j_error_type":"SD"})");
  ASSERT_TRUE(mean.options);
  EXPECT_EQ(mean.options->fit.error, pr::MeanErrorKind::Sd);
  EXPECT_FALSE(mean.sd_replaced);
}

// ---- "Unchanged" (R16) -------------------------------------------------------

// A flat JSON object (no comma or colon inside a value) with its members in
// the reverse order and a space after each colon and comma.
std::string reordered_and_spaced(const std::string& json) {
  std::vector<std::string> members;
  std::string member;
  for (const char c : json.substr(1, json.size() - 2)) {
    if (c == ',') {
      members.push_back(member);
      member.clear();
    } else {
      member += c;
      if (c == ':') member += ' ';
    }
  }
  members.push_back(member);
  std::string out = "{";
  for (auto it = members.rbegin(); it != members.rend(); ++it) out += (it == members.rbegin() ? "" : ", ") + *it;
  return out + "}";
}

// A jsonb column gives the options back with its own spacing and key order,
// and the version that saved is no part of the fit.
TEST(FluxSameValue, OptionsCompareAsJsonWithoutTheSoftware) {
  FluxOptions o;
  o.fit.kind = pr::ModelKind::Bowl;
  const MonitorSet kuiper = default_monitor_sets().sets[0];
  ps::FluxValue a;
  a.j = 1.0e-3;
  a.j_err = 2.0e-7;
  a.monitor_name = kuiper.name;
  a.options_json = flux_options_json(o, kuiper, true, false, false, 1.12, 5, "pychron-cpp 0.4.0");
  EXPECT_TRUE(same_flux_value(a, a));

  // As PostgreSQL returns a jsonb: its own key order, a space after each colon and comma.
  ps::FluxValue stored = a;
  stored.options_json = reordered_and_spaced(*a.options_json);
  ASSERT_NE(*stored.options_json, *a.options_json);
  ASSERT_TRUE(has(*stored.options_json, R"("model_kind": "Bowl", )")) << *stored.options_json;
  ASSERT_EQ(parse_flux_options(*stored.options_json).options, std::optional<FluxOptions>(o));
  EXPECT_TRUE(same_flux_value(a, stored));
  EXPECT_TRUE(same_flux_value(stored, a));

  ps::FluxValue newer = a;  // a version bump and nothing else
  newer.options_json = flux_options_json(o, kuiper, true, false, false, 1.12, 5, "pychron-cpp 0.5.0");
  EXPECT_TRUE(same_flux_value(a, newer));
  EXPECT_TRUE(same_flux_value(stored, newer));

  ps::FluxValue plane = a;
  FluxOptions p = o;
  p.fit.kind = pr::ModelKind::Plane;
  plane.options_json = flux_options_json(p, kuiper, true, false, false, 1.12, 5, "pychron-cpp 0.4.0");
  EXPECT_FALSE(same_flux_value(a, plane));
  EXPECT_FALSE(same_flux_value(stored, plane));

  // Every other key of the options counts, and every other field.
  ps::FluxValue excluded = a;
  excluded.options_json = flux_options_json(o, kuiper, true, true, false, 1.12, 5, "pychron-cpp 0.4.0");
  EXPECT_FALSE(same_flux_value(a, excluded));
  ps::FluxValue other_j = stored;
  other_j.j = 1.1e-3;
  EXPECT_FALSE(same_flux_value(a, other_j));
  ps::FluxValue omitted = stored;
  omitted.analyses = {{std::nullopt, "66001-01", true}};
  EXPECT_FALSE(same_flux_value(a, omitted));
}

TEST(FluxSameValue, OptionsThatAreNotJsonCompareAsText) {
  ps::FluxValue a, b;
  EXPECT_TRUE(same_flux_value(a, b));  // neither has options
  a.options_json = "not json";
  EXPECT_FALSE(same_flux_value(a, b));
  b.options_json = "not json";
  EXPECT_TRUE(same_flux_value(a, b));
  b.options_json = "not  json";
  EXPECT_FALSE(same_flux_value(a, b));
  b.options_json = "{}";
  EXPECT_FALSE(same_flux_value(a, b));
  a.options_json = R"({"software":"x"})";  // only the software differs
  EXPECT_TRUE(same_flux_value(a, b));
  a.options_json = "[1, 2]";  // not an object: nothing to leave out
  b.options_json = "[1,2]";
  EXPECT_TRUE(same_flux_value(a, b));
}

// ---- load_level (design section 6.1) ----------------------------------------

class FluxLoadLevel : public testing::FluxStoreTest {
 protected:
  void SetUp() override {
    FluxStoreTest::SetUp();
    if (HasFatalFailure()) return;
    seed_level(analysed());
  }

  // How many of the eight monitor holes are seeded with their analyses.
  virtual int analysed() const { return 8; }

  Result<LevelInputs> load(const MonitorSelection& selection = {}, const std::string& level = "A",
                           const std::string& irradiation = "NM-300") {
    StoreSource* opened = source();
    if (!opened) return fail(ErrorKind::Config, "test: the source did not open");
    return load_level(*opened, store(), irradiation, level, selection);
  }

  // The position at hole `n`; a failure and an empty position when there is none.
  static const LevelPosition& hole(const LevelInputs& in, int n) {
    for (const auto& p : in.positions)
      if (p.hole == n) return p;
    ADD_FAILURE() << "no position at hole " << n;
    static const LevelPosition none;
    return none;
  }

  static FluxOptions plane_sem() {
    FluxOptions o;
    o.fit.kind = pr::ModelKind::Plane;
    o.fit.error = pr::MeanErrorKind::Sem;
    o.mean_error = pr::MeanErrorKind::Sem;
    return o;
  }

  // A saved J with options naming a monitor set.
  static ps::FluxValue saved_with(const FluxOptions& options, const MonitorSet& set, bool used_in_fit = true,
                                  bool excluded = false) {
    ps::FluxValue v;
    v.j = 1.0e-3;
    v.j_err = 2.0e-7;
    v.options_json = flux_options_json(options, set, used_in_fit, excluded, false, 1.12, 5, "test");
    return v;
  }
};

TEST_P(FluxLoadLevel, PositionsMonitorsAndGeometry) {
  auto in = load();
  ASSERT_TRUE(in) << to_string(in.error());
  EXPECT_EQ(in->irradiation, "NM-300");
  EXPECT_EQ(in->level, "A");
  EXPECT_EQ(in->holder, "12-hole");
  EXPECT_EQ(in->monitor_set, default_monitor_sets().sets[0]);
  EXPECT_FALSE(in->saved_options);
  EXPECT_FALSE(in->saved_sd_replaced);
  ASSERT_EQ(in->positions.size(), 12u);
  for (int h = 1; h <= 12; ++h) {
    const LevelPosition& p = in->positions[static_cast<std::size_t>(h - 1)];
    EXPECT_EQ(p.hole, h);
    EXPECT_EQ(p.position_uuid, seeded().positions.at(h).str());
    EXPECT_FALSE(p.saved) << h;
    const bool ring = h <= 8;
    EXPECT_EQ(p.monitor, ring) << h;
    EXPECT_EQ(p.sample, ring ? "FC-2" : "unk");
    EXPECT_EQ(p.identifier, std::to_string(ring ? 66000 + h : 66100 + h - 8));
    const double x = ring ? flux_golden::kRing[h - 1].x : flux_golden::kPoints[h - 9].x;
    const double y = ring ? flux_golden::kRing[h - 1].y : flux_golden::kPoints[h - 9].y;
    EXPECT_DOUBLE_EQ(p.x, x) << h;
    EXPECT_DOUBLE_EQ(p.y, y) << h;
    if (!ring) {
      EXPECT_TRUE(p.analyses.empty()) << h;
      continue;
    }
    ASSERT_EQ(p.analyses.size(), 3u) << h;
    for (int k = 1; k <= 3; ++k) {
      const LevelAnalysis& a = p.analyses[static_cast<std::size_t>(k - 1)];
      const std::string record = p.identifier + "-0" + std::to_string(k);
      EXPECT_EQ(a.record_id, record);
      EXPECT_EQ(a.uuid, seeded().analyses.at(record).str());
      EXPECT_EQ(a.tag, "ok");
      ASSERT_TRUE(a.f) << record << ": " << a.reduction_error;
      EXPECT_TRUE(a.reduction_error.empty()) << a.reduction_error;
      const double f = testing::seed_f(testing::seed_j(h, k));
      EXPECT_NEAR(a.f->nominal(), f, f * 1e-5) << record;
      EXPECT_GT(a.f->std_dev(), 0.0) << record;
      EXPECT_LT(a.f->std_dev(), f * 2e-3) << record;
    }
  }

  // The seeded level is the golden ring: a plane through its mean J predicts
  // the golden J at holes 9-12.
  auto fit = fit_level(*in, plane_sem(), {});
  ASSERT_TRUE(fit) << to_string(fit.error());
  const auto& golden = flux_golden::kCases[0];  // plane_unweighted_sem
  for (int i = 0; i < 4; ++i)
    EXPECT_NEAR(fit->positions[static_cast<std::size_t>(8 + i)].j, golden.j[i], golden.j[i] * 1e-5) << i;
}

TEST_P(FluxLoadLevel, GeometryIsByOrdinalNotByHoleId) {
  // Position N is the hole with ordinal N - 1 (as on the entry sheet). The
  // labels run the other way ("12" is the first hole) and the holes are
  // stored last first: neither the label nor the place in the list decides.
  auto holes = testing::seed_holes();
  for (auto& h : holes) h.hole_id = std::to_string(12 - h.ordinal);
  std::reverse(holes.begin(), holes.end());
  ASSERT_EQ(holes.front().ordinal, 11);
  ASSERT_EQ(holes.front().hole_id, "1");
  publish_holder(holes);
  auto in = load();
  ASSERT_TRUE(in) << to_string(in.error());
  ASSERT_EQ(in->positions.size(), 12u);
  for (int h = 1; h <= 12; ++h) {
    const LevelPosition& p = in->positions[static_cast<std::size_t>(h - 1)];
    EXPECT_EQ(p.hole, h);
    EXPECT_DOUBLE_EQ(p.x, h <= 8 ? flux_golden::kRing[h - 1].x : flux_golden::kPoints[h - 9].x) << h;
    EXPECT_DOUBLE_EQ(p.y, h <= 8 ? flux_golden::kRing[h - 1].y : flux_golden::kPoints[h - 9].y) << h;
  }

  // Labels that are no numbers at all.
  holes = testing::seed_holes();
  for (auto& h : holes) h.hole_id = "A" + std::to_string(h.ordinal + 1);
  publish_holder(holes);
  auto lettered = load();
  ASSERT_TRUE(lettered) << to_string(lettered.error());
  ASSERT_EQ(lettered->positions.size(), 12u);
  EXPECT_DOUBLE_EQ(lettered->positions[2].x, flux_golden::kRing[2].x);
  EXPECT_DOUBLE_EQ(lettered->positions[11].y, flux_golden::kPoints[3].y);
}

TEST_P(FluxLoadLevel, MonitorSetResolution) {
  const MonitorSets defaults = default_monitor_sets();
  // None named, nothing saved: the document's default.
  auto fresh = load();
  ASSERT_TRUE(fresh) << to_string(fresh.error());
  EXPECT_EQ(fresh->monitor_set.name, "FC-2 (Kuiper 2008)");
  EXPECT_EQ(fresh->saved_monitor_set, "");
  EXPECT_FALSE(fresh->saved_monitor_set_missing);

  // A saved fit names the set it used.
  save_flux(5, saved_with(plane_sem(), defaults.sets[1]));
  auto saved = load();
  ASSERT_TRUE(saved) << to_string(saved.error());
  EXPECT_EQ(saved->monitor_set, defaults.sets[1]);
  EXPECT_EQ(saved->saved_monitor_set, "FC-2 (Renne 1998)");
  EXPECT_FALSE(saved->saved_monitor_set_missing);

  // A set named explicitly wins over the saved fit's.
  MonitorSelection kuiper;
  kuiper.monitor_set = "FC-2 (Kuiper 2008)";
  auto named = load(kuiper);
  ASSERT_TRUE(named) << to_string(named.error());
  EXPECT_EQ(named->monitor_set, defaults.sets[0]);
  EXPECT_EQ(named->saved_monitor_set, "FC-2 (Renne 1998)");
  EXPECT_FALSE(named->saved_monitor_set_missing);

  // A name that does not exist is an error listing the ones that do.
  MonitorSelection nope;
  nope.monitor_set = "GA-1550";
  auto unknown = load(nope);
  ASSERT_FALSE(unknown);
  EXPECT_EQ(unknown.error().kind, ErrorKind::Config);
  EXPECT_TRUE(has(unknown.error().what, "GA-1550")) << unknown.error().what;
  EXPECT_TRUE(has(unknown.error().what, "FC-2 (Kuiper 2008)")) << unknown.error().what;
  EXPECT_TRUE(has(unknown.error().what, "FC-2 (Renne 1998)")) << unknown.error().what;

  // A later saved fit naming a set the document does not have (an imported
  // "FC Min") falls through to the default, and the level says so (R17):
  // the standard is not changed silently.
  ps::FluxValue legacy;
  legacy.j = 1.0e-3;
  legacy.j_err = 2.0e-7;
  legacy.options_json = R"({"model_kind":"Plane","monitor_reference":"FC Min"})";
  save_flux(7, legacy);
  auto fallen = load();
  ASSERT_TRUE(fallen) << to_string(fallen.error());
  EXPECT_EQ(fallen->monitor_set.name, "FC-2 (Kuiper 2008)");
  EXPECT_EQ(fallen->saved_monitor_set, "FC Min");
  EXPECT_TRUE(fallen->saved_monitor_set_missing);
  // The store still lacks it when the caller names a set; the caller chose.
  auto chosen = load(kuiper);
  ASSERT_TRUE(chosen) << to_string(chosen.error());
  EXPECT_EQ(chosen->monitor_set, defaults.sets[0]);
  EXPECT_EQ(chosen->saved_monitor_set, "FC Min");
  EXPECT_TRUE(chosen->saved_monitor_set_missing);

  // The newest saved fit decides, wherever it is: a later save on a lower
  // hole than the ones above, then one on a hole between them.
  save_flux(2, saved_with(plane_sem(), defaults.sets[1]));
  auto newest = load();
  ASSERT_TRUE(newest) << to_string(newest.error());
  EXPECT_EQ(newest->monitor_set.name, "FC-2 (Renne 1998)");
  EXPECT_EQ(newest->saved_monitor_set, "FC-2 (Renne 1998)");
  EXPECT_FALSE(newest->saved_monitor_set_missing);
  FluxOptions bowl;
  bowl.fit.kind = pr::ModelKind::Bowl;
  save_flux(6, saved_with(bowl, defaults.sets[0]));
  save_flux(1, saved_with(plane_sem(), defaults.sets[1]));
  auto lowest_last = load();
  ASSERT_TRUE(lowest_last) << to_string(lowest_last.error());
  EXPECT_EQ(lowest_last->monitor_set.name, "FC-2 (Renne 1998)");
  ASSERT_TRUE(lowest_last->saved_options);
  EXPECT_EQ(*lowest_last->saved_options, plane_sem());
  save_flux(4, saved_with(bowl, defaults.sets[0]));
  auto middle_last = load();
  ASSERT_TRUE(middle_last) << to_string(middle_last.error());
  EXPECT_EQ(middle_last->monitor_set.name, "FC-2 (Kuiper 2008)");
  ASSERT_TRUE(middle_last->saved_options);
  EXPECT_EQ(*middle_last->saved_options, bowl);
}

TEST_P(FluxLoadLevel, SampleOverrideAndAllPositions) {
  MonitorSelection unk;
  unk.sample = "unk";
  auto in = load(unk);
  ASSERT_TRUE(in) << to_string(in.error());
  EXPECT_EQ(in->monitor_set.name, "FC-2 (Kuiper 2008)");
  EXPECT_EQ(in->monitor_set.sample, "unk");
  EXPECT_FALSE(in->all_positions);
  ASSERT_EQ(in->positions.size(), 12u);
  for (const auto& p : in->positions) {
    EXPECT_EQ(p.monitor, p.hole >= 9) << p.hole;
    EXPECT_TRUE(p.analyses.empty()) << p.hole;  // 9-12 have none; 1-8 are not monitors
  }

  MonitorSelection all;
  all.all_positions = true;
  auto every = load(all);
  ASSERT_TRUE(every) << to_string(every.error());
  EXPECT_TRUE(every->all_positions);
  ASSERT_EQ(every->positions.size(), 8u);  // 9-12 have no analyses: neither monitors nor unknowns
  int h = 1;
  for (const auto& p : every->positions) {
    EXPECT_EQ(p.hole, h++);
    EXPECT_TRUE(p.monitor) << p.hole;
    EXPECT_EQ(p.analyses.size(), 3u) << p.hole;
  }

  // With every position a monitor, the sample does not matter.
  all.sample = "unk";
  auto both = load(all);
  ASSERT_TRUE(both) << to_string(both.error());
  EXPECT_EQ(both->positions.size(), 8u);
}

TEST_P(FluxLoadLevel, AnEmptySampleOverrideIsRefused) {
  // A hole with no sample and no identifier: an empty name would match it.
  ASSERT_TRUE(store().add_irradiation_position(seeded().acquisition_client, {seeded().level, 13}));
  auto holes = testing::seed_holes();
  holes.push_back({12, "13", 1.0, 1.0, 1.0});
  publish_holder(holes);
  MonitorSelection empty;
  empty.sample = "";
  auto refused = load(empty);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().kind, ErrorKind::Config);
  EXPECT_EQ(refused.error().what, "flux: the monitor sample name is empty");
  auto fine = load();
  ASSERT_TRUE(fine) << to_string(fine.error());
  EXPECT_EQ(fine->positions.size(), 12u);  // the empty hole is neither a monitor nor an unknown
}

TEST_P(FluxLoadLevel, ReadsTheSavedFit) {
  FluxOptions options;
  options.fit.kind = pr::ModelKind::Plane;
  options.fit.weighted = true;
  options.fit.error = pr::MeanErrorKind::Sem;
  options.mean = pr::MeanKind::Weighted;
  options.mean_error = pr::MeanErrorKind::Sd;
  ps::FluxValue v = saved_with(options, default_monitor_sets().sets[1], false, true);  // the user left it out
  v.j = 1.0031e-3;
  v.j_err = 2.5e-7;
  v.mean_j = 1.0029e-3;
  v.mean_j_err = 3.5e-7;
  v.mean_j_mswd = 1.7;
  v.analyses = {{seeded().analyses.at("66003-01"), "66003-01", false},
                {seeded().analyses.at("66003-02"), "66003-02", true},
                {std::nullopt, "66003-03", true}};
  const ps::Uuid revision = save_flux(3, v);
  ps::FluxValue unknown;
  unknown.j = 1.0e-3;
  unknown.j_err = 1.0e-7;
  const ps::Uuid unknown_revision = save_flux(10, unknown);

  auto in = load();
  ASSERT_TRUE(in) << to_string(in.error());
  ASSERT_EQ(in->positions.size(), 12u);
  const LevelPosition& p = hole(*in, 3);
  ASSERT_TRUE(p.saved);
  const SavedFlux& s = *p.saved;
  EXPECT_EQ(s.revision, revision.str());
  EXPECT_EQ(s.j, std::optional<double>(1.0031e-3));
  EXPECT_EQ(s.j_err, std::optional<double>(2.5e-7));
  EXPECT_EQ(s.mean_j, std::optional<double>(1.0029e-3));
  EXPECT_EQ(s.mean_j_err, std::optional<double>(3.5e-7));
  EXPECT_EQ(s.mean_j_mswd, std::optional<double>(1.7));
  ASSERT_TRUE(s.options);
  EXPECT_EQ(*s.options, options);
  EXPECT_EQ(s.used_in_fit, std::optional<bool>(false));
  EXPECT_EQ(s.excluded, std::optional<bool>(true));
  EXPECT_EQ(s.monitor_set, "FC-2 (Renne 1998)");
  EXPECT_EQ(s.omitted, (std::set<std::string>{"66003-02", "66003-03"}));
  EXPECT_EQ(s.saved_by, "jsmith");
  EXPECT_NE(ps::UtcTime::parse(s.saved_utc), std::nullopt) << s.saved_utc;

  const LevelPosition& u = hole(*in, 10);
  ASSERT_TRUE(u.saved);
  EXPECT_EQ(u.saved->revision, unknown_revision.str());
  EXPECT_EQ(u.saved->j, std::optional<double>(1.0e-3));
  EXPECT_FALSE(u.saved->options);
  for (int h : {1, 2, 4, 9, 12}) EXPECT_FALSE(hole(*in, h).saved) << h;

  ASSERT_TRUE(in->saved_options);
  EXPECT_EQ(*in->saved_options, options);
  EXPECT_FALSE(in->saved_sd_replaced);
  EXPECT_EQ(in->monitor_set.name, "FC-2 (Renne 1998)");

  // The saved fit shapes the next one: hole 3 stays out and its omissions hold.
  auto fit = fit_level(*in, *in->saved_options, {});
  ASSERT_TRUE(fit) << to_string(fit.error());
  const FittedPosition& fitted = fit->positions[2];
  EXPECT_FALSE(fitted.used_in_fit);
  EXPECT_TRUE(fitted.excluded);
  EXPECT_EQ(fitted.n, 1);
  EXPECT_EQ(fitted.saved_revision, std::optional<std::string>(revision.str()));

  // A newer revision of the position replaces what is read.
  const ps::Uuid newer = save_flux(3, saved_with(plane_sem(), default_monitor_sets().sets[0]));
  auto again = load();
  ASSERT_TRUE(again) << to_string(again.error());
  ASSERT_TRUE(hole(*again, 3).saved);
  EXPECT_EQ(hole(*again, 3).saved->revision, newer.str());
  EXPECT_TRUE(hole(*again, 3).saved->omitted.empty());
  ASSERT_TRUE(again->saved_options);
  EXPECT_EQ(*again->saved_options, plane_sem());
  EXPECT_EQ(again->monitor_set.name, "FC-2 (Kuiper 2008)");
}

TEST_P(FluxLoadLevel, ASavedRevisionThatIsOnlyAJ) {
  for (int h = 1; h <= 12; ++h) {
    ps::FluxValue v;
    v.j = 1.0e-3 + h * 1e-6;
    if (h != 5) v.j_err = 1.0e-6;  // hole 5: a J with no error, with which its analyses have no age
    save_flux(h, v);
  }
  auto in = load();
  ASSERT_TRUE(in) << to_string(in.error());
  ASSERT_EQ(in->positions.size(), 12u);
  for (const auto& p : in->positions) {
    ASSERT_TRUE(p.saved) << p.hole;
    EXPECT_EQ(p.saved->j, std::optional<double>(1.0e-3 + p.hole * 1e-6));
    EXPECT_EQ(p.saved->j_err, p.hole == 5 ? std::nullopt : std::optional<double>(1.0e-6));
    EXPECT_FALSE(p.saved->mean_j);
    EXPECT_FALSE(p.saved->options);
    EXPECT_FALSE(p.saved->used_in_fit.has_value());
    EXPECT_TRUE(p.saved->monitor_set.empty());
    EXPECT_TRUE(p.saved->omitted.empty());
    EXPECT_FALSE(p.saved->revision.empty());
    // F needs no J, and does not change with the one saved.
    for (const auto& a : p.analyses) ASSERT_TRUE(a.f) << a.record_id << ": " << a.reduction_error;
  }
  EXPECT_FALSE(in->saved_options);
  EXPECT_FALSE(in->saved_sd_replaced);
  EXPECT_EQ(in->monitor_set.name, "FC-2 (Kuiper 2008)");

  auto fit = fit_level(*in, FluxOptions{}, {});
  ASSERT_TRUE(fit) << to_string(fit.error());
  ASSERT_EQ(fit->positions.size(), 12u);
  for (const auto& p : fit->positions) {
    EXPECT_EQ(p.used_in_fit, p.monitor) << p.hole;
    EXPECT_TRUE(p.dev_percent.has_value()) << p.hole;
  }
}

TEST_P(FluxLoadLevel, TagsAreCarried) {
  tag("66001-02", "omit");
  tag("66002-01", "invalid");  // loaded all the same: a tag omits, it does not hide
  auto in = load();
  ASSERT_TRUE(in) << to_string(in.error());
  const LevelPosition& one = hole(*in, 1);
  ASSERT_EQ(one.analyses.size(), 3u);
  EXPECT_EQ(one.analyses[0].tag, "ok");
  EXPECT_EQ(one.analyses[1].record_id, "66001-02");
  EXPECT_EQ(one.analyses[1].tag, "omit");
  EXPECT_TRUE(one.analyses[1].f);
  const LevelPosition& two = hole(*in, 2);
  ASSERT_EQ(two.analyses.size(), 3u);
  EXPECT_EQ(two.analyses[0].record_id, "66002-01");
  EXPECT_EQ(two.analyses[0].tag, "invalid");

  auto fit = fit_level(*in, plane_sem(), {});
  ASSERT_TRUE(fit) << to_string(fit.error());
  EXPECT_EQ(fit->positions[0].n, 2);
  EXPECT_TRUE(fit->positions[0].analyses[1].omitted);
  EXPECT_EQ(fit->positions[1].n, 2);
  EXPECT_EQ(fit->positions[2].n, 3);
}

TEST_P(FluxLoadLevel, ErrorsNameTheCause) {
  auto no_irradiation = load({}, "A", "NM-999");
  ASSERT_FALSE(no_irradiation);
  EXPECT_EQ(no_irradiation.error().kind, ErrorKind::Config);
  EXPECT_TRUE(has(no_irradiation.error().what, "NM-999")) << no_irradiation.error().what;

  auto no_level = load({}, "Z");
  ASSERT_FALSE(no_level);
  EXPECT_EQ(no_level.error().kind, ErrorKind::Config);
  EXPECT_TRUE(has(no_level.error().what, "level Z")) << no_level.error().what;
  EXPECT_TRUE(has(no_level.error().what, "NM-300")) << no_level.error().what;

  ASSERT_TRUE(store().add_level(seeded().acquisition_client, {seeded().irradiation, "B"}));
  auto no_holder = load({}, "B");
  ASSERT_FALSE(no_holder);
  EXPECT_EQ(no_holder.error().kind, ErrorKind::Config);
  EXPECT_TRUE(has(no_holder.error().what, "level B of NM-300 has no holder")) << no_holder.error().what;

  // The holder loses its last hole; the level still has a position there.
  // The label "12" on another hole does not stand in for it.
  auto holes = testing::seed_holes();
  holes.pop_back();
  holes.front().hole_id = "12";
  publish_holder(holes);
  auto no_hole = load();
  ASSERT_FALSE(no_hole);
  EXPECT_EQ(no_hole.error().kind, ErrorKind::Config);
  EXPECT_EQ(no_hole.error().what, "flux: position 12 of level A of NM-300 is beyond holder 12-hole (11 holes)");
}

TEST_P(FluxLoadLevel, AnImportedLegacyLevelLoadsAndRefits) {
  // As the importer writes a level file's positions (meta_adapter.cpp,
  // meta_layout.cpp flux_value): the legacy options dict verbatim, the
  // analyses with is_omitted, the decay constant the fit used.
  const std::string options =
      R"({"model_kind": "Plane", "monitor_sample_name": "FC-2", "monte_carlo_ntrials": 100000,
          "predicted_j_error_type": "SD", "use_monte_carlo": false, "use_weighted_fit": false,
          "monitor_reference": "FC Min"})";
  for (int h = 1; h <= 12; ++h) {
    const bool ring = h <= 8;
    ps::FluxValue v;
    v.j = ring ? flux_golden::kRing[h - 1].j : 1.0e-3;
    v.j_err = 2.0e-7;
    v.mean_j = ring ? flux_golden::kRing[h - 1].j : 0.0;
    v.mean_j_err = ring ? 3.0e-7 : 0.0;
    v.position_jerr = 0.0;
    v.lambda_k_total = 5.464e-10;
    v.lambda_k_total_err = 0.0;
    v.monitor_name = "FC-2";
    v.monitor_material = "sanidine";
    v.monitor_age = 28.201;
    v.monitor_age_err = 0.0;
    v.options_json = options;
    if (ring) {
      const std::string id = std::to_string(66000 + h);
      for (int k = 1; k <= 3; ++k) {
        const std::string record = id + "-0" + std::to_string(k);
        v.analyses.push_back({seeded().analyses.at(record), record, h == 4 && k == 3});
      }
    }
    save_flux(h, v);
  }

  auto in = load();
  ASSERT_TRUE(in) << to_string(in.error());
  ASSERT_EQ(in->positions.size(), 12u);
  // "FC Min" is no set of the document: the default, and the level says so.
  EXPECT_EQ(in->monitor_set.name, "FC-2 (Kuiper 2008)");
  EXPECT_EQ(in->saved_monitor_set, "FC Min");
  EXPECT_TRUE(in->saved_monitor_set_missing);
  // Plane saved with SD reads as Msem, and says so (F13).
  ASSERT_TRUE(in->saved_options);
  EXPECT_EQ(in->saved_options->fit.kind, pr::ModelKind::Plane);
  EXPECT_FALSE(in->saved_options->fit.weighted);
  EXPECT_EQ(in->saved_options->fit.error, pr::MeanErrorKind::Msem);
  EXPECT_TRUE(in->saved_sd_replaced);
  for (const auto& p : in->positions) {
    ASSERT_TRUE(p.saved) << p.hole;
    ASSERT_TRUE(p.saved->options) << p.hole;
    EXPECT_EQ(p.saved->monitor_set, "FC Min");
    EXPECT_FALSE(p.saved->used_in_fit.has_value());
    EXPECT_EQ(p.saved->omitted, p.hole == 4 ? std::set<std::string>{"66004-03"} : std::set<std::string>{}) << p.hole;
    if (p.monitor) {
      ASSERT_EQ(p.analyses.size(), 3u);
      for (const auto& a : p.analyses) ASSERT_TRUE(a.f) << a.record_id << ": " << a.reduction_error;
    }
  }

  auto fit = fit_level(*in, *in->saved_options, {});
  ASSERT_TRUE(fit) << to_string(fit.error());
  ASSERT_EQ(fit->positions.size(), 12u);
  EXPECT_EQ(fit->dof, 5);
  for (const auto& p : fit->positions) {
    EXPECT_EQ(p.used_in_fit, p.monitor) << p.hole;
    EXPECT_EQ(p.n, !p.monitor ? 0 : p.hole == 4 ? 2 : 3) << p.hole;
    ASSERT_TRUE(p.dev_percent.has_value()) << p.hole;
    if (p.monitor) {
      EXPECT_LT(std::abs(*p.dev_percent), 0.5) << p.hole;  // the plane is close to the saved ring J
    }
  }
}

// ---- save_level (design section 6.3) ----------------------------------------

class FluxSaveLevel : public FluxLoadLevel {
 protected:
  // The level loaded and fitted with a plane; an empty fit on a failure.
  LevelFit fitted(const Edits& edits = {}) {
    auto in = load();
    if (!in) {
      ADD_FAILURE() << to_string(in.error());
      return {};
    }
    auto fit = fit_level(*in, plane_sem(), edits);
    if (!fit) {
      ADD_FAILURE() << to_string(fit.error());
      return {};
    }
    return std::move(*fit);
  }

  FluxSaveOutcome save(const LevelFit& fit, const SaveSelection& selection = {}) {
    auto outcome = save_level(store(), actor(), fit, selection, "test");
    if (!outcome) {
      ADD_FAILURE() << to_string(outcome.error());
      return {};
    }
    return std::move(*outcome);
  }

  // The flux_position reference object of a hole; nullopt when there is none.
  std::optional<ps::Uuid> object(int hole) {
    auto found = store().find_catalog_row(ps::CatalogTable::RefObject,
                                          {std::string("flux_position"), "NM-300/A/" + std::to_string(hole)});
    EXPECT_TRUE(found) << (found ? "" : to_string(found.error()));
    return found ? *found : std::nullopt;
  }

  std::optional<ps::Uuid> head(int hole) {
    const auto subject = object(hole);
    if (!subject) return std::nullopt;
    auto revision = store().head(*subject, ps::Kind::RefValue);
    EXPECT_TRUE(revision) << (revision ? "" : to_string(revision.error()));
    return revision ? *revision : std::nullopt;
  }

  // The FluxValue at a hole's head; a failure and an empty value when there is none.
  ps::FluxValue head_value(int hole) {
    const auto revision = head(hole);
    if (!revision) {
      ADD_FAILURE() << "hole " << hole << " has no flux";
      return {};
    }
    auto payload = store().load_payload(*revision);
    if (!payload || !*payload) {
      ADD_FAILURE() << "hole " << hole << " has no payload";
      return {};
    }
    const auto* ref = std::get_if<ps::RefPayload>(&**payload);
    const auto* flux = ref ? std::get_if<ps::FluxValue>(ref) : nullptr;
    if (!flux) {
      ADD_FAILURE() << "hole " << hole << " is not a flux";
      return {};
    }
    return *flux;
  }

  ps::ChangeSeq change_seq() {
    auto seq = store().latest_change_seq();
    EXPECT_TRUE(seq) << (seq ? "" : to_string(seq.error()));
    return seq ? *seq : -1;
  }

  // An analysis of an unknown of the level, with F = 30.
  ps::Uuid ingest_unknown(const std::string& identifier, const std::string& timestamp) {
    auto analysis = testing::seed_ingest_monitor(store(), seeded(), identifier, 1, 30.0, timestamp);
    EXPECT_TRUE(analysis) << (analysis ? "" : to_string(analysis.error()));
    return analysis ? *analysis : ps::Uuid{};
  }

  // The age of an analysis as the source reduces it now; nullopt when it has none.
  std::optional<double> age(const ps::Uuid& analysis) {
    StoreSource* opened = source();
    if (!opened) return std::nullopt;
    auto refreshed = opened->refresh();
    EXPECT_TRUE(refreshed) << (refreshed ? "" : to_string(refreshed.error()));
    auto loaded = opened->load(analysis.str());
    if (!loaded) {
      ADD_FAILURE() << to_string(loaded.error());
      return std::nullopt;
    }
    const ReducedPtr reduced = reduce_analysis(*loaded, ReductionSettings{});
    if (!reduced->arar || !reduced->arar->ages) return std::nullopt;
    return reduced->arar->ages->age.nominal();
  }

  // The store's sets with a third, "Second": the second default set renamed,
  // with age 99 Ma and sample "unk" (what holes 9-12 carry).
  void add_second_set() {
    auto sets = load_monitor_sets(store());
    ASSERT_TRUE(sets) << to_string(sets.error());
    MonitorSets edited = sets->sets;
    MonitorSet second = edited.sets[1];
    second.name = "Second";
    second.sample = "unk";
    second.age_ma = 99.0;
    edited.sets.push_back(second);
    auto committed = save_monitor_sets(store(), actor(), edited, *sets);
    ASSERT_TRUE(committed) << to_string(committed.error());
    ASSERT_TRUE(std::holds_alternative<ps::Committed>(*committed));
  }

  static ps::FluxValue only_j(double j) {
    ps::FluxValue v;
    v.j = j;
    v.j_err = j * 1e-3;
    return v;
  }
};

TEST_P(FluxSaveLevel, WritesOneRevisionPerPositionInOneChangeset) {
  Edits edits;
  edits.omit = {"66003-02"};
  const LevelFit fit = fitted(edits);
  ASSERT_EQ(fit.positions.size(), 12u);
  const ps::ChangeSeq before = change_seq();

  const FluxSaveOutcome outcome = save(fit);
  EXPECT_EQ(outcome.written, 12);
  EXPECT_EQ(outcome.unchanged, 0);
  EXPECT_EQ(outcome.skipped, 0);
  EXPECT_FALSE(outcome.conflict);
  EXPECT_EQ(outcome.conflict_position, "");
  // The new reference objects are catalog changes; the fit is one changeset.
  auto changes = store().changes_since(before, 100);
  ASSERT_TRUE(changes) << to_string(changes.error());
  EXPECT_FALSE(changes->more);
  int changeset_entries = 0, catalog_entries = 0;
  for (const auto& entry : changes->entries) {
    if (entry.kind == "changeset") ++changeset_entries;
    else if (entry.kind == "catalog") ++catalog_entries;
  }
  EXPECT_EQ(changeset_entries, 1);
  EXPECT_EQ(changeset_entries + catalog_entries, static_cast<int>(changes->entries.size()));

  std::set<ps::Uuid> changesets;
  for (const FittedPosition& p : fit.positions) {
    SCOPED_TRACE("hole " + std::to_string(p.hole));
    const ps::FluxValue v = head_value(p.hole);
    const ps::FluxValue expected = flux_value_of(fit, p, "test");
    EXPECT_EQ(json_of(v.options_json), json_of(expected.options_json));
    ps::FluxValue rest = v, expected_rest = expected;  // every other field exactly
    rest.options_json.reset();
    expected_rest.options_json.reset();
    EXPECT_EQ(rest, expected_rest);
    EXPECT_EQ(v.j, std::optional<double>(p.j));
    EXPECT_EQ(v.j_err, std::optional<double>(p.j_err));
    if (p.monitor) {
      ASSERT_TRUE(p.mean_j);
      EXPECT_EQ(v.mean_j, p.mean_j);
      EXPECT_EQ(v.mean_j_err, p.mean_j_err);
      EXPECT_EQ(v.mean_j_mswd, p.mean_j_mswd);
      EXPECT_TRUE(v.mean_j_err);
      EXPECT_TRUE(v.mean_j_mswd);
      ASSERT_EQ(v.analyses.size(), 3u);
      for (int aliquot = 1; aliquot <= 3; ++aliquot) {
        const std::string record_id = p.identifier + "-0" + std::to_string(aliquot);
        const ps::FluxAnalysis& a = v.analyses[static_cast<std::size_t>(aliquot - 1)];
        EXPECT_EQ(a.record_id, record_id);
        EXPECT_EQ(a.analysis, std::optional<ps::Uuid>(seeded().analyses.at(record_id)));
        EXPECT_EQ(a.is_omitted, record_id == "66003-02") << record_id;
      }
    } else {
      EXPECT_EQ(v.mean_j, std::nullopt);
      EXPECT_EQ(v.mean_j_err, std::nullopt);
      EXPECT_EQ(v.mean_j_mswd, std::nullopt);
      EXPECT_TRUE(v.analyses.empty());
    }
    ASSERT_TRUE(v.lambda_k_total);
    EXPECT_DOUBLE_EQ(*v.lambda_k_total, 5.463e-10);
    ASSERT_TRUE(v.lambda_k_total_err);
    EXPECT_DOUBLE_EQ(*v.lambda_k_total_err, std::hypot(9.9e-13, 1.4e-12));
    EXPECT_EQ(v.monitor_name, std::optional<std::string>("FC-2 (Kuiper 2008)"));
    EXPECT_EQ(v.monitor_material, std::optional<std::string>("sanidine"));
    EXPECT_EQ(v.monitor_age, std::optional<double>(28.201));
    EXPECT_EQ(v.monitor_age_err, std::optional<double>(0.046));
    EXPECT_EQ(v.position_jerr, std::nullopt);  // F5
    EXPECT_EQ(v.extra_json, std::nullopt);
    ASSERT_TRUE(v.options_json);
    EXPECT_EQ(json_of(v.options_json),
              json_of(flux_options_json(plane_sem(), fit.monitor_set, p.used_in_fit, false, false, fit.mswd, fit.dof, "test")));
    const FluxOptionsDoc doc = parse_flux_options(*v.options_json);
    EXPECT_EQ(doc.options, std::optional<FluxOptions>(plane_sem()));
    EXPECT_EQ(doc.used_in_fit, std::optional<bool>(p.monitor));
    EXPECT_EQ(doc.excluded, std::optional<bool>(false));  // an unknown is not used, and nobody excluded it

    const auto subject = object(p.hole);
    ASSERT_TRUE(subject);
    auto history = store().history(*subject, ps::Kind::RefValue);
    ASSERT_TRUE(history) << to_string(history.error());
    ASSERT_EQ(history->size(), 1u);
    EXPECT_EQ(history->front().parent, std::nullopt);
    EXPECT_EQ(history->front().changeset.message, "fit flux for NM-300A");
    EXPECT_EQ(history->front().changeset.kind, ps::ChangesetKind::Reference);
    EXPECT_EQ(history->front().changeset.author_user, actor().user);
    changesets.insert(history->front().changeset.uuid);
  }
  EXPECT_EQ(changesets.size(), 1u);
}

TEST_P(FluxSaveLevel, ThenLoadShowsTheSavedJ) {
  const LevelFit fit = fitted();
  ASSERT_EQ(save(fit).written, 12);

  auto in = load();
  ASSERT_TRUE(in) << to_string(in.error());
  ASSERT_EQ(in->positions.size(), 12u);
  for (const FittedPosition& p : fit.positions) {
    SCOPED_TRACE("hole " + std::to_string(p.hole));
    const LevelPosition& loaded = hole(*in, p.hole);
    ASSERT_TRUE(loaded.saved);
    EXPECT_EQ(loaded.saved->j, std::optional<double>(p.j));
    EXPECT_EQ(loaded.saved->j_err, std::optional<double>(p.j_err));
    EXPECT_EQ(loaded.saved->mean_j, p.mean_j);
    EXPECT_EQ(loaded.saved->options, std::optional<FluxOptions>(plane_sem()));
    EXPECT_EQ(loaded.saved->used_in_fit, std::optional<bool>(p.used_in_fit));
    EXPECT_EQ(loaded.saved->excluded, std::optional<bool>(false));
    EXPECT_EQ(loaded.saved->monitor_set, "FC-2 (Kuiper 2008)");
    EXPECT_EQ(loaded.saved->saved_by, "jsmith");
    const auto revision = head(p.hole);
    ASSERT_TRUE(revision);
    EXPECT_EQ(loaded.saved->revision, revision->str());
  }
  EXPECT_EQ(in->saved_options, std::optional<FluxOptions>(plane_sem()));
  EXPECT_EQ(in->monitor_set.name, "FC-2 (Kuiper 2008)");
}

TEST_P(FluxSaveLevel, ASecondSaveWritesNothing) {
  const LevelFit fit = fitted();
  ASSERT_EQ(save(fit).written, 12);
  const ps::ChangeSeq saved = change_seq();

  // The same fit again, though it was made before any flux was saved.
  const FluxSaveOutcome same = save(fit);
  EXPECT_EQ(same.written, 0);
  EXPECT_EQ(same.unchanged, 12);
  EXPECT_EQ(same.skipped, 0);
  EXPECT_FALSE(same.conflict);
  EXPECT_EQ(change_seq(), saved);

  // And the level loaded and fitted again from what was saved.
  const LevelFit again = fitted();
  ASSERT_EQ(again.positions.size(), 12u);
  EXPECT_TRUE(again.positions[0].saved_revision);
  const FluxSaveOutcome refit = save(again);
  EXPECT_EQ(refit.written, 0);
  EXPECT_EQ(refit.unchanged, 12);
  EXPECT_FALSE(refit.conflict);
  EXPECT_EQ(change_seq(), saved);
  for (int n = 1; n <= 12; ++n) {
    const auto subject = object(n);
    ASSERT_TRUE(subject) << n;
    auto history = store().history(*subject, ps::Kind::RefValue);
    ASSERT_TRUE(history) << to_string(history.error());
    EXPECT_EQ(history->size(), 1u) << n;
  }
}

// R16 (revises R3): the software that saved is no part of the fit, so a new
// version saving the same fit writes nothing.
TEST_P(FluxSaveLevel, ASaveByAnotherVersionWritesNothing) {
  const LevelFit fit = fitted();
  ASSERT_EQ(save(fit).written, 12);
  const ps::ChangeSeq saved = change_seq();
  auto outcome = save_level(store(), actor(), fitted(), {}, "pychron-cpp 9.9.9");
  ASSERT_TRUE(outcome) << to_string(outcome.error());
  EXPECT_EQ(outcome->written, 0);
  EXPECT_EQ(outcome->unchanged, 12);
  EXPECT_FALSE(outcome->conflict);
  EXPECT_EQ(change_seq(), saved);
  EXPECT_EQ(json_of(head_value(1).options_json).value("software", ""), "test");
}

TEST_P(FluxSaveLevel, OmissionsAndExclusionsSurviveSaveAndRefit) {
  Edits edits;
  edits.omit = {"66002-03"};
  edits.exclude_positions = {5};
  const LevelFit first = fitted(edits);
  ASSERT_EQ(first.positions.size(), 12u);
  EXPECT_FALSE(first.positions[4].used_in_fit);
  EXPECT_TRUE(first.positions[4].excluded);
  EXPECT_TRUE(first.positions[1].analyses[2].omitted);
  ASSERT_EQ(save(first).written, 12);
  // Only the position the user left out says so: the unknowns are not used either.
  for (int n = 1; n <= 12; ++n) {
    const FluxOptionsDoc doc = parse_flux_options(head_value(n).options_json.value_or(""));
    EXPECT_EQ(doc.excluded, std::optional<bool>(n == 5)) << n;
    EXPECT_EQ(doc.used_in_fit, std::optional<bool>(n <= 8 && n != 5)) << n;
  }

  const LevelFit second = fitted();  // no edits: the saved fit's hold
  ASSERT_EQ(second.positions.size(), 12u);
  EXPECT_EQ(second.options, first.options);
  EXPECT_EQ(second.mswd, first.mswd);
  EXPECT_EQ(second.dof, first.dof);
  EXPECT_EQ(second.parameters, first.parameters);
  for (std::size_t i = 0; i < 12; ++i) {
    SCOPED_TRACE("hole " + std::to_string(i + 1));
    const FittedPosition& a = first.positions[i];
    const FittedPosition& b = second.positions[i];
    EXPECT_EQ(b.j, a.j);
    EXPECT_EQ(b.j_err, a.j_err);
    EXPECT_EQ(b.mean_j, a.mean_j);
    EXPECT_EQ(b.mean_j_err, a.mean_j_err);
    EXPECT_EQ(b.n, a.n);
    EXPECT_EQ(b.used_in_fit, a.used_in_fit);
    EXPECT_EQ(b.excluded, a.excluded);
    ASSERT_EQ(b.analyses.size(), a.analyses.size());
    for (std::size_t k = 0; k < a.analyses.size(); ++k) EXPECT_EQ(b.analyses[k].omitted, a.analyses[k].omitted) << k;
    EXPECT_EQ(b.saved_j, std::optional<double>(a.j));
  }
  EXPECT_EQ(save(second).unchanged, 12);

  Edits reset;
  reset.reset_omits = true;
  const LevelFit third = fitted(reset);
  ASSERT_EQ(third.positions.size(), 12u);
  EXPECT_TRUE(third.positions[4].used_in_fit);
  EXPECT_FALSE(third.positions[4].excluded);
  EXPECT_FALSE(third.positions[1].analyses[2].omitted);
  EXPECT_EQ(third.positions[1].n, 3);
  EXPECT_NE(third.dof, first.dof);
  EXPECT_NE(third.positions[8].j, first.positions[8].j);
}

// R15: what a save says of an analysis that could not be used is not an
// omission, so the analysis is back once it reduces.
TEST_P(FluxSaveLevel, AnAnalysisNotReducedAtSaveIsNotOmittedAfterwards) {
  auto in = load();
  ASSERT_TRUE(in) << to_string(in.error());
  LevelInputs broken = *in;
  ASSERT_EQ(broken.positions[2].analyses.size(), 3u);
  broken.positions[2].analyses[1].f.reset();
  broken.positions[2].analyses[1].reduction_error = "no isotopes";
  auto fit = fit_level(broken, plane_sem(), {});
  ASSERT_TRUE(fit) << to_string(fit.error());
  EXPECT_EQ(fit->positions[2].n, 2);
  ASSERT_EQ(save(*fit).written, 12);
  const ps::FluxValue saved = head_value(3);
  ASSERT_EQ(saved.analyses.size(), 3u);
  for (const auto& a : saved.analyses) EXPECT_FALSE(a.is_omitted) << a.record_id;

  const LevelFit again = fitted();  // it reduces now
  ASSERT_EQ(again.positions.size(), 12u);
  EXPECT_EQ(again.positions[2].n, 3);
  for (const auto& a : again.positions[2].analyses) EXPECT_FALSE(a.omitted) << a.record_id;
}

TEST_P(FluxSaveLevel, SkippedPositionsKeepTheirHead) {
  const ps::Uuid kept = save_flux(9, only_j(1.0e-3));
  const LevelFit fit = fitted();
  const FluxSaveOutcome outcome = save(fit, SaveSelection{{9}});
  EXPECT_EQ(outcome.written, 11);
  EXPECT_EQ(outcome.unchanged, 0);
  EXPECT_EQ(outcome.skipped, 1);
  EXPECT_FALSE(outcome.conflict);
  EXPECT_EQ(head(9), std::optional<ps::Uuid>(kept));
  EXPECT_EQ(head_value(9), only_j(1.0e-3));
  for (int n : {1, 8, 10, 12}) EXPECT_EQ(head_value(n).j, std::optional<double>(fit.positions[static_cast<std::size_t>(n - 1)].j)) << n;

  // A hole that is not on the level is no position to skip.
  EXPECT_EQ(save(fit, SaveSelection{{9, 40}}).skipped, 1);
}

TEST_P(FluxSaveLevel, APositionWithNoReferenceObjectGetsOne) {
  const auto keys = [&] {
    std::set<std::string> out;
    auto objects = store().ref_objects(ps::RefType::FluxPosition, seeded().irradiation);
    EXPECT_TRUE(objects) << (objects ? "" : to_string(objects.error()));
    if (objects)
      for (const auto& o : *objects) out.insert(o.key);
    return out;
  };
  EXPECT_TRUE(keys().empty());
  const ps::Uuid analysis = ingest_unknown("66101", "2026-01-01T19:01:00Z");
  const LevelFit fit = fitted();
  ASSERT_EQ(save(fit).written, 12);

  const std::set<std::string> after = keys();
  EXPECT_EQ(after.size(), 12u);
  EXPECT_TRUE(after.contains("NM-300/A/9"));
  auto objects = store().ref_objects(ps::RefType::FluxPosition, seeded().irradiation);
  ASSERT_TRUE(objects) << to_string(objects.error());
  for (const auto& o : *objects) {
    EXPECT_EQ(o.irradiation, std::optional<ps::Uuid>(seeded().irradiation)) << o.key;
    EXPECT_EQ(o.level, std::optional<ps::Uuid>(seeded().level)) << o.key;
    EXPECT_TRUE(o.head) << o.key;
  }
  // Scoped to its position: it is the flux of the analyses run on it.
  auto refs = store().resolve_refs(analysis, ps::RefPolicy{});
  ASSERT_TRUE(refs) << to_string(refs.error());
  const auto flux = std::find_if(refs->refs.begin(), refs->refs.end(),
                                 [](const ps::ResolvedRef& r) { return r.type == ps::RefType::FluxPosition; });
  ASSERT_NE(flux, refs->refs.end());
  EXPECT_EQ(flux->key, "NM-300/A/9");
  EXPECT_EQ(std::optional<ps::Uuid>(flux->revision), head(9));
}

TEST_P(FluxSaveLevel, AMovedHeadIsAConflictAndNothingIsWritten) {
  ASSERT_EQ(save(fitted()).written, 12);
  Edits edits;
  edits.omit = {"66001-02"};
  const LevelFit fit = fitted(edits);  // another J at every position, on the heads just saved
  ASSERT_EQ(fit.positions.size(), 12u);
  const std::optional<ps::Uuid> expected = head(7);
  ASSERT_TRUE(expected);
  EXPECT_EQ(fit.positions[6].saved_revision, std::optional<std::string>(expected->str()));

  const ps::Uuid moved = save_flux(7, only_j(1.0e-3));  // someone else saves hole 7
  std::map<int, std::optional<ps::Uuid>> heads;
  for (int n = 1; n <= 12; ++n) heads[n] = head(n);
  const ps::ChangeSeq before = change_seq();

  const FluxSaveOutcome outcome = save(fit);
  ASSERT_TRUE(outcome.conflict);
  EXPECT_EQ(outcome.conflict_position, "hole 7");
  EXPECT_EQ(outcome.written, 0);
  EXPECT_EQ(outcome.conflict->subject, *object(7));
  EXPECT_EQ(outcome.conflict->kind, ps::Kind::RefValue);
  EXPECT_EQ(outcome.conflict->expected, expected);
  EXPECT_EQ(outcome.conflict->actual, std::optional<ps::Uuid>(moved));
  for (int n = 1; n <= 12; ++n) EXPECT_EQ(head(n), heads[n]) << n;
  EXPECT_EQ(head(7), std::optional<ps::Uuid>(moved));
  EXPECT_EQ(change_seq(), before);

  // Loaded again, the same fit saves.
  const FluxSaveOutcome retried = save(fitted(edits));
  EXPECT_FALSE(retried.conflict);
  EXPECT_EQ(retried.written, 12);
}

TEST_P(FluxSaveLevel, AnUnknownsAgeChangesAndAPinnedOneDoesNot) {
  const ps::Uuid free = ingest_unknown("66101", "2026-01-01T19:01:00Z");
  const ps::Uuid pinned = ingest_unknown("66102", "2026-01-01T19:02:00Z");
  save_flux(9, only_j(1.0e-3));
  const ps::Uuid old = save_flux(10, only_j(1.0e-3));
  {
    const auto flux = object(10);
    ASSERT_TRUE(flux);
    auto uow = store().begin(actor());
    ASSERT_TRUE(uow) << to_string(uow.error());
    ASSERT_TRUE((*uow)->add_revision(pinned, ps::Kind::RefPins, ps::RevisionPayload{ps::RefPins{{*flux, old}}},
                                     std::nullopt));
    auto committed = (*uow)->commit(ps::ChangesetKind::Reduction, "<FLUX_FREEZE>");
    ASSERT_TRUE(committed) << to_string(committed.error());
    ASSERT_TRUE(std::holds_alternative<ps::Committed>(*committed));
  }
  const std::optional<double> free_before = age(free);
  const std::optional<double> pinned_before = age(pinned);
  ASSERT_TRUE(free_before);
  ASSERT_TRUE(pinned_before);
  EXPECT_GT(*free_before, 0.0);

  const LevelFit fit = fitted();
  ASSERT_EQ(fit.positions.size(), 12u);
  ASSERT_NE(fit.positions[8].j, 1.0e-3);
  ASSERT_NE(fit.positions[9].j, 1.0e-3);
  ASSERT_EQ(save(fit).written, 12);
  EXPECT_NE(head(10), std::optional<ps::Uuid>(old));

  const std::optional<double> free_after = age(free);
  const std::optional<double> pinned_after = age(pinned);
  ASSERT_TRUE(free_after);
  ASSERT_TRUE(pinned_after);
  EXPECT_NE(*free_after, *free_before);
  EXPECT_EQ(*pinned_after, *pinned_before);
}

// R19: a neighbour model can extrapolate to a J that is no J. None is saved.
TEST_P(FluxSaveLevel, AJThatIsNotPositiveAndFiniteRefusesTheWholeSave) {
  const LevelFit good = fitted();
  ASSERT_EQ(good.positions.size(), 12u);
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  struct Case {
    const char* what;
    int hole;
    void (*spoil)(FittedPosition&, double);
    double value;
    const char* names;
  };
  const Case cases[] = {
      {"negative J", 10, [](FittedPosition& p, double v) { p.j = v; }, -1.0e-4, "J"},
      {"zero J", 10, [](FittedPosition& p, double v) { p.j = v; }, 0.0, "J"},
      {"NaN J", 2, [](FittedPosition& p, double v) { p.j = v; }, nan, "J"},
      {"infinite J", 12, [](FittedPosition& p, double v) { p.j = v; }, inf, "J"},
      {"negative J error", 9, [](FittedPosition& p, double v) { p.j_err = v; }, -1.0e-7, "J error"},
      {"NaN J error", 9, [](FittedPosition& p, double v) { p.j_err = v; }, nan, "J error"},
      {"zero mean J", 3, [](FittedPosition& p, double v) { p.mean_j = v; }, 0.0, "mean J"},
      {"NaN mean J error", 3, [](FittedPosition& p, double v) { p.mean_j_err = v; }, nan, "mean J error"},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.what);
    LevelFit fit = good;
    c.spoil(fit.positions[static_cast<std::size_t>(c.hole - 1)], c.value);
    const ps::ChangeSeq before = change_seq();
    auto outcome = save_level(store(), actor(), fit, {}, "test");
    ASSERT_FALSE(outcome);
    EXPECT_EQ(outcome.error().kind, ErrorKind::Config);
    EXPECT_TRUE(outcome.error().what.starts_with("flux: hole " + std::to_string(c.hole) + " of NM-300A has ")) << outcome.error().what;
    EXPECT_TRUE(has(outcome.error().what, std::string(" ") + c.names + " ")) << outcome.error().what;
    EXPECT_TRUE(has(outcome.error().what, "nothing was saved")) << outcome.error().what;
    EXPECT_EQ(change_seq(), before);  // not even a reference object for a position
    for (int n = 1; n <= 12; ++n) EXPECT_FALSE(object(n)) << n;
  }

  // The lowest such hole is the one named.
  LevelFit two = good;
  two.positions[10].j = -1.0;
  two.positions[4].j = 0.0;
  auto outcome = save_level(store(), actor(), two, {}, "test");
  ASSERT_FALSE(outcome);
  EXPECT_TRUE(outcome.error().what.starts_with("flux: hole 5 of NM-300A has J 0")) << outcome.error().what;

  // A position that is not saved is not looked at; a J with no error is a J.
  LevelFit skipped = good;
  skipped.positions[9].j = -1.0e-4;
  skipped.positions[8].j_err = 0.0;
  const ps::ChangeSeq before = change_seq();
  auto saved = save_level(store(), actor(), skipped, SaveSelection{{10}}, "test");
  ASSERT_TRUE(saved) << to_string(saved.error());
  EXPECT_EQ(saved->written, 11);
  EXPECT_EQ(saved->skipped, 1);
  EXPECT_GT(change_seq(), before);
  EXPECT_FALSE(object(10));
}

// ---- A saved fit's monitors are the next fit's (R18, spec F9) ----------------

TEST_P(FluxSaveLevel, ASampleOverrideIsSavedAndUsedAgain) {
  // The store's sets name a sample this level does not hold.
  auto sets = load_monitor_sets(store());
  ASSERT_TRUE(sets) << to_string(sets.error());
  MonitorSets edited = sets->sets;
  for (auto& set : edited.sets) set.sample = "FCT";
  auto committed = save_monitor_sets(store(), actor(), edited, *sets);
  ASSERT_TRUE(committed) << to_string(committed.error());
  auto plain = load();
  ASSERT_TRUE(plain) << to_string(plain.error());
  EXPECT_EQ(plain->monitor_set.sample, "FCT");
  for (const auto& p : plain->positions) EXPECT_FALSE(p.monitor) << p.hole;

  MonitorSelection fc2;
  fc2.sample = "FC-2";
  auto in = load(fc2);
  ASSERT_TRUE(in) << to_string(in.error());
  auto fit = fit_level(*in, plane_sem(), {});
  ASSERT_TRUE(fit) << to_string(fit.error());
  EXPECT_EQ(fit->dof, 5);
  ASSERT_EQ(save(*fit).written, 12);
  EXPECT_EQ(parse_flux_options(head_value(1).options_json.value_or("")).monitor_sample, "FC-2");

  // No selection: the saved fit's sample, so the same monitors.
  auto again = load();
  ASSERT_TRUE(again) << to_string(again.error());
  EXPECT_EQ(again->monitor_set.name, "FC-2 (Kuiper 2008)");
  EXPECT_EQ(again->monitor_set.sample, "FC-2");
  ASSERT_EQ(again->positions.size(), 12u);
  for (const auto& p : again->positions) EXPECT_EQ(p.monitor, p.hole <= 8) << p.hole;
  auto refit = fit_level(*again, plane_sem(), {});
  ASSERT_TRUE(refit) << to_string(refit.error());
  EXPECT_EQ(refit->parameters, fit->parameters);
  EXPECT_EQ(save(*refit).unchanged, 12);

  // A sample named wins over the saved one.
  MonitorSelection fct;
  fct.sample = "FCT";
  auto named = load(fct);
  ASSERT_TRUE(named) << to_string(named.error());
  EXPECT_EQ(named->monitor_set.sample, "FCT");
  for (const auto& p : named->positions) EXPECT_FALSE(p.monitor) << p.hole;

  // R20: the override belongs to the set it was saved under. That set named
  // explicitly keeps it; another set uses its own sample, and a sample
  // named with it still wins.
  MonitorSelection same_set;
  same_set.monitor_set = "FC-2 (Kuiper 2008)";
  auto same = load(same_set);
  ASSERT_TRUE(same) << to_string(same.error());
  EXPECT_EQ(same->monitor_set.sample, "FC-2");
  MonitorSelection other_set;
  other_set.monitor_set = "FC-2 (Renne 1998)";
  auto other = load(other_set);
  ASSERT_TRUE(other) << to_string(other.error());
  EXPECT_EQ(other->monitor_set.name, "FC-2 (Renne 1998)");
  EXPECT_EQ(other->monitor_set.sample, "FCT");
  for (const auto& p : other->positions) EXPECT_FALSE(p.monitor) << p.hole;
  other_set.sample = "FC-2";
  auto both = load(other_set);
  ASSERT_TRUE(both) << to_string(both.error());
  EXPECT_EQ(both->monitor_set.name, "FC-2 (Renne 1998)");
  EXPECT_EQ(both->monitor_set.sample, "FC-2");
}

// R20 (narrows R18): every save writes the monitor sample, so it must not
// follow the level to another standard: FC-2 positions fitted with another
// set's age would be a wrong J with no word said.
TEST_P(FluxSaveLevel, AnotherMonitorSetUsesItsOwnSample) {
  ASSERT_EQ(save(fitted()).written, 12);  // a plain save, the default set: monitor_sample "FC-2"
  ASSERT_EQ(parse_flux_options(head_value(1).options_json.value_or("")).monitor_sample, "FC-2");

  auto sets = load_monitor_sets(store());
  ASSERT_TRUE(sets) << to_string(sets.error());
  MonitorSets edited = sets->sets;
  MonitorSet second = edited.sets[1];
  second.name = "Second";
  second.sample = "unk";  // what holes 9-12 carry
  second.age_ma = 99.0;
  edited.sets.push_back(second);
  ASSERT_TRUE(save_monitor_sets(store(), actor(), edited, *sets));

  MonitorSelection other;
  other.monitor_set = "Second";
  auto in = load(other);
  ASSERT_TRUE(in) << to_string(in.error());
  EXPECT_EQ(in->monitor_set.name, "Second");
  EXPECT_EQ(in->monitor_set.age_ma, 99.0);
  EXPECT_EQ(in->monitor_set.sample, "unk");
  EXPECT_EQ(in->saved_monitor_set, "FC-2 (Kuiper 2008)");
  ASSERT_EQ(in->positions.size(), 12u);
  for (const auto& p : in->positions) EXPECT_EQ(p.monitor, p.hole >= 9) << p.hole;

  // No set named: the saved fit's, and its sample, as before.
  auto plain = load();
  ASSERT_TRUE(plain) << to_string(plain.error());
  EXPECT_EQ(plain->monitor_set.name, "FC-2 (Kuiper 2008)");
  EXPECT_EQ(plain->monitor_set.sample, "FC-2");
  for (const auto& p : plain->positions) EXPECT_EQ(p.monitor, p.hole <= 8) << p.hole;
}

// R20: the default that stands in for a set the store lacks (R17) is another
// standard too, and uses its own sample.
TEST_P(FluxSaveLevel, TheFallbackDefaultSetUsesItsOwnSample) {
  ps::FluxValue gone;
  gone.j = 1.0e-3;
  gone.j_err = 2.0e-7;
  gone.options_json = R"({"model_kind":"Plane","monitor_reference":"FC Min","monitor_sample":"unk"})";
  save_flux(3, gone);
  auto in = load();
  ASSERT_TRUE(in) << to_string(in.error());
  EXPECT_EQ(in->monitor_set.name, "FC-2 (Kuiper 2008)");
  EXPECT_EQ(in->monitor_set.sample, "FC-2");
  EXPECT_EQ(in->saved_monitor_set, "FC Min");
  EXPECT_TRUE(in->saved_monitor_set_missing);
  for (const auto& p : in->positions) EXPECT_EQ(p.monitor, p.hole <= 8) << p.hole;
}

TEST_P(FluxSaveLevel, AllPositionsIsSavedAndUsedAgain) {
  ingest_unknown("66101", "2026-01-01T19:01:00Z");  // hole 9 has an analysis: a monitor with every position
  MonitorSelection all;
  all.all_positions = true;
  auto in = load(all);
  ASSERT_TRUE(in) << to_string(in.error());
  ASSERT_EQ(in->positions.size(), 9u);
  auto fit = fit_level(*in, plane_sem(), {});
  ASSERT_TRUE(fit) << to_string(fit.error());
  EXPECT_TRUE(fit->all_positions);
  EXPECT_EQ(fit->dof, 6);  // 9 monitors, 3 parameters
  ASSERT_EQ(save(*fit).written, 9);
  for (int n = 1; n <= 9; ++n)
    EXPECT_EQ(parse_flux_options(head_value(n).options_json.value_or("")).all_positions, std::optional<bool>(true)) << n;

  // No selection: as saved.
  auto again = load();
  ASSERT_TRUE(again) << to_string(again.error());
  EXPECT_TRUE(again->all_positions);
  ASSERT_EQ(again->positions.size(), 9u);
  for (const auto& p : again->positions) EXPECT_TRUE(p.monitor) << p.hole;
  auto refit = fit_level(*again, plane_sem(), {});
  ASSERT_TRUE(refit) << to_string(refit.error());
  EXPECT_EQ(refit->parameters, fit->parameters);
  EXPECT_EQ(save(*refit).unchanged, 9);

  // R20: a sample named, with no word on the positions, selects by sample;
  // with every position asked for as well, it does not.
  MonitorSelection by_name;
  by_name.sample = "FC-2";
  auto sampled = load(by_name);
  ASSERT_TRUE(sampled) << to_string(sampled.error());
  EXPECT_FALSE(sampled->all_positions);
  ASSERT_EQ(sampled->positions.size(), 12u);
  for (const auto& p : sampled->positions) EXPECT_EQ(p.monitor, p.hole <= 8) << p.hole;
  by_name.all_positions = true;
  auto still_all = load(by_name);
  ASSERT_TRUE(still_all) << to_string(still_all.error());
  EXPECT_TRUE(still_all->all_positions);
  EXPECT_EQ(still_all->positions.size(), 9u);

  // The caller can ask for the monitor sample's positions again.
  MonitorSelection sample_based;
  sample_based.all_positions = false;
  auto restored = load(sample_based);
  ASSERT_TRUE(restored) << to_string(restored.error());
  EXPECT_FALSE(restored->all_positions);
  ASSERT_EQ(restored->positions.size(), 12u);
  for (const auto& p : restored->positions) EXPECT_EQ(p.monitor, p.hole <= 8) << p.hole;
  auto by_sample = fit_level(*restored, plane_sem(), {});
  ASSERT_TRUE(by_sample) << to_string(by_sample.error());
  EXPECT_FALSE(by_sample->all_positions);
  EXPECT_EQ(by_sample->dof, 5);
  // Saved so, the newest fit decides: sample based again.
  ASSERT_EQ(save(*by_sample).written, 12);
  auto last = load();
  ASSERT_TRUE(last) << to_string(last.error());
  EXPECT_FALSE(last->all_positions);
  EXPECT_EQ(last->positions.size(), 12u);
}


// R21: a saved fit's set, sample and all_positions are one revision's. A
// level saved here whose heads on some positions were later replaced by
// imported revisions naming another set, with no monitor_sample and no
// all_positions, is the newer fit's: that set with its own sample, not the
// older native fit's sample (or all_positions) under the newer set.
TEST_P(FluxSaveLevel, MixedHeadsTakeSetSampleAndAllPositionsFromOneRevision) {
  ASSERT_NO_FATAL_FAILURE(add_second_set());
  MonitorSelection fc2;
  fc2.sample = "FC-2";
  auto in = load(fc2);
  ASSERT_TRUE(in) << to_string(in.error());
  auto fit = fit_level(*in, plane_sem(), {});
  ASSERT_TRUE(fit) << to_string(fit.error());
  ASSERT_EQ(save(*fit).written, 12);
  ASSERT_EQ(parse_flux_options(head_value(1).options_json.value_or("")).monitor_sample, "FC-2");

  ps::FluxValue imported;
  imported.j = 1.0e-3;
  imported.j_err = 2.0e-7;
  imported.options_json = R"({"model_kind":"Plane","monitor_reference":"Second"})";
  save_flux(10, imported);
  save_flux(11, imported);

  auto mixed = load();
  ASSERT_TRUE(mixed) << to_string(mixed.error());
  EXPECT_EQ(mixed->saved_monitor_set, "Second");
  EXPECT_EQ(mixed->monitor_set.name, "Second");
  EXPECT_EQ(mixed->monitor_set.sample, "unk");
  EXPECT_FALSE(mixed->all_positions);
  ASSERT_EQ(mixed->positions.size(), 12u);
  for (const auto& p : mixed->positions) EXPECT_EQ(p.monitor, p.hole >= 9) << p.hole;
}

TEST_P(FluxSaveLevel, MixedHeadsDoNotCarryAnOlderAllPositions) {
  ASSERT_NO_FATAL_FAILURE(add_second_set());
  ingest_unknown("66101", "2026-01-01T19:01:00Z");
  MonitorSelection all;
  all.all_positions = true;
  auto in = load(all);
  ASSERT_TRUE(in) << to_string(in.error());
  auto fit = fit_level(*in, plane_sem(), {});
  ASSERT_TRUE(fit) << to_string(fit.error());
  ASSERT_EQ(save(*fit).written, 9);

  ps::FluxValue imported;
  imported.j = 1.0e-3;
  imported.j_err = 2.0e-7;
  imported.options_json = R"({"model_kind":"Plane","monitor_reference":"Second","monitor_sample":"unk"})";
  save_flux(12, imported);

  auto mixed = load();
  ASSERT_TRUE(mixed) << to_string(mixed.error());
  EXPECT_EQ(mixed->monitor_set.name, "Second");
  EXPECT_EQ(mixed->monitor_set.sample, "unk");
  EXPECT_FALSE(mixed->all_positions);
  ASSERT_EQ(mixed->positions.size(), 12u);
  for (const auto& p : mixed->positions) EXPECT_EQ(p.monitor, p.hole >= 9) << p.hole;
}

// R22 (as R20 for the sample): a saved all-positions fit is a choice made
// under its own standard. Another set, named or the default standing in for
// a set the store lacks, selects by its own sample unless --all-positions
// is given again.
TEST_P(FluxSaveLevel, AnotherMonitorSetDoesNotCarryAllPositions) {
  ASSERT_NO_FATAL_FAILURE(add_second_set());
  ingest_unknown("66101", "2026-01-01T19:01:00Z");
  MonitorSelection all;
  all.all_positions = true;
  auto in = load(all);
  ASSERT_TRUE(in) << to_string(in.error());
  auto fit = fit_level(*in, plane_sem(), {});
  ASSERT_TRUE(fit) << to_string(fit.error());
  ASSERT_EQ(save(*fit).written, 9);

  MonitorSelection other;
  other.monitor_set = "Second";
  auto second = load(other);
  ASSERT_TRUE(second) << to_string(second.error());
  EXPECT_EQ(second->monitor_set.name, "Second");
  EXPECT_FALSE(second->all_positions);
  ASSERT_EQ(second->positions.size(), 12u);
  for (const auto& p : second->positions) EXPECT_EQ(p.monitor, p.hole >= 9) << p.hole;

  // Asked for again, every position.
  other.all_positions = true;
  auto again = load(other);
  ASSERT_TRUE(again) << to_string(again.error());
  EXPECT_TRUE(again->all_positions);
  EXPECT_EQ(again->positions.size(), 9u);

  // The saved set named explicitly is no other set: as saved.
  MonitorSelection same;
  same.monitor_set = "FC-2 (Kuiper 2008)";
  auto kuiper = load(same);
  ASSERT_TRUE(kuiper) << to_string(kuiper.error());
  EXPECT_TRUE(kuiper->all_positions);
  EXPECT_EQ(kuiper->positions.size(), 9u);
}

TEST_P(FluxSaveLevel, TheFallbackDefaultSetDoesNotCarryAllPositions) {
  ingest_unknown("66101", "2026-01-01T19:01:00Z");
  ps::FluxValue gone;
  gone.j = 1.0e-3;
  gone.j_err = 2.0e-7;
  gone.options_json = R"({"model_kind":"Plane","monitor_reference":"FC Min","monitor_sample":"FC-2","all_positions":true})";
  save_flux(3, gone);
  auto in = load();
  ASSERT_TRUE(in) << to_string(in.error());
  EXPECT_TRUE(in->saved_monitor_set_missing);
  EXPECT_FALSE(in->all_positions);
  EXPECT_EQ(in->positions.size(), 12u);
}

// Under another set, then saved: the level's saved fit is now that set's,
// and a plain reload repeats it (with that set's sample).
TEST_P(FluxSaveLevel, AFitUnderAnotherSetSavedThenReloadedKeepsThatSet) {
  ASSERT_EQ(save(fitted()).written, 12);
  ASSERT_NO_FATAL_FAILURE(add_second_set());
  MonitorSelection other;
  other.monitor_set = "Second";
  FluxOptions mean;
  mean.fit.kind = pr::ModelKind::WeightedMean;
  mean.fit.error = pr::MeanErrorKind::Sem;
  // Holes 9-12 are monitors under "Second"; hole 9 gets an analysis.
  ingest_unknown("66101", "2026-01-01T19:01:00Z");
  auto in = load(other);
  ASSERT_TRUE(in) << to_string(in.error());
  auto fit = fit_level(*in, mean, {});
  ASSERT_TRUE(fit) << to_string(fit.error());
  const FluxSaveOutcome saved = save(*fit);
  EXPECT_EQ(saved.written, 12);
  EXPECT_FALSE(saved.conflict);
  const FluxOptionsDoc doc = parse_flux_options(head_value(1).options_json.value_or(""));
  EXPECT_EQ(doc.monitor_set, "Second");
  EXPECT_EQ(doc.monitor_sample, "unk");

  auto plain = load();
  ASSERT_TRUE(plain) << to_string(plain.error());
  EXPECT_EQ(plain->monitor_set.name, "Second");
  EXPECT_EQ(plain->monitor_set.age_ma, 99.0);
  EXPECT_EQ(plain->monitor_set.sample, "unk");
  EXPECT_EQ(plain->saved_monitor_set, "Second");
  EXPECT_FALSE(plain->saved_monitor_set_missing);
  ASSERT_TRUE(plain->saved_options);
  EXPECT_EQ(plain->saved_options->fit.kind, pr::ModelKind::WeightedMean);
  for (const auto& p : plain->positions) EXPECT_EQ(p.monitor, p.hole >= 9) << p.hole;
  auto refit = fit_level(*plain, *plain->saved_options, {});
  ASSERT_TRUE(refit) << to_string(refit.error());
  const FluxSaveOutcome again = save(*refit);
  EXPECT_EQ(again.written, 0);
  EXPECT_EQ(again.unchanged, 12);
}

// ---- A level saved before all its monitors were measured (R15) ---------------

class FluxSaveSparseLevel : public FluxSaveLevel {
 protected:
  int analysed() const override { return 6; }  // holes 7 and 8 have no analyses yet
};

TEST_P(FluxSaveSparseLevel, MonitorsMeasuredAfterASaveJoinTheFit) {
  const LevelFit first = fitted();
  ASSERT_EQ(first.positions.size(), 12u);
  EXPECT_EQ(first.dof, 3);  // 6 monitors, 3 parameters
  for (int n : {7, 8}) {
    const FittedPosition& p = first.positions[static_cast<std::size_t>(n - 1)];
    EXPECT_TRUE(p.monitor) << n;
    EXPECT_FALSE(p.used_in_fit) << n;
    EXPECT_FALSE(p.excluded) << n;
    EXPECT_FALSE(p.mean_j) << n;
  }
  ASSERT_EQ(save(first).written, 12);
  for (int n : {7, 8}) {
    const FluxOptionsDoc doc = parse_flux_options(head_value(n).options_json.value_or(""));
    EXPECT_EQ(doc.used_in_fit, std::optional<bool>(false)) << n;
    EXPECT_EQ(doc.excluded, std::optional<bool>(false)) << n;
  }

  add_monitor_analyses(7);
  add_monitor_analyses(8);
  const LevelFit second = fitted();  // no edits: nobody left 7 and 8 out
  ASSERT_EQ(second.positions.size(), 12u);
  for (const FittedPosition& p : second.positions) {
    EXPECT_EQ(p.used_in_fit, p.monitor) << p.hole;
    EXPECT_EQ(p.n, p.monitor ? 3 : 0) << p.hole;
    EXPECT_FALSE(p.excluded) << p.hole;
  }
  EXPECT_EQ(second.dof, 5);  // as for 8
}

// A revision saved before `excluded` existed carries no such key: a monitor
// it left unused for want of analyses is used once it has them.
TEST_P(FluxSaveSparseLevel, AnOlderSaveWithoutTheKeyDoesNotExcludeEither) {
  ps::FluxValue older;
  older.j = 1.0e-3;
  older.j_err = 2.0e-7;
  older.options_json = R"j({"model_kind":"Plane","predicted_j_error_type":"sem","error_kind":"sem",
                            "monitor_reference":"FC-2 (Kuiper 2008)","monitor_sample":"FC-2","used_in_fit":false})j";
  save_flux(7, older);
  older.mean_j = 1.0e-3;  // it had its mean and still was not used: the user left it out
  older.mean_j_err = 2.0e-7;
  save_flux(6, older);
  add_monitor_analyses(7);
  const LevelFit fit = fitted();
  ASSERT_EQ(fit.positions.size(), 12u);
  EXPECT_TRUE(fit.positions[6].used_in_fit);
  EXPECT_FALSE(fit.positions[6].excluded);
  EXPECT_FALSE(fit.positions[5].used_in_fit);
  EXPECT_TRUE(fit.positions[5].excluded);
  EXPECT_FALSE(fit.positions[7].used_in_fit);  // still no analyses
  EXPECT_FALSE(fit.positions[7].excluded);
}

// ---- The status of a level (flux window design, section 4.4) -----------------

class FluxLevelStatus : public FluxSaveLevel {
 protected:
  // The sheet of a level of NM-300; a failure and an empty sheet when there is none.
  ps::LevelSheet sheet(const std::string& level = "A") {
    auto levels = store().levels(seeded().irradiation);
    if (!levels) {
      ADD_FAILURE() << to_string(levels.error());
      return {};
    }
    for (const auto& row : *levels) {
      if (row.name != level) continue;
      auto found = store().level_sheet(row.uuid);
      if (!found || !*found) {
        ADD_FAILURE() << "no sheet of level " << level;
        return {};
      }
      return **found;
    }
    ADD_FAILURE() << "no level " << level;
    return {};
  }
};

TEST_P(FluxLevelStatus, NotFittedUntilEveryMonitorHasAJ) {
  EXPECT_EQ(level_flux_status(sheet(), "FC-2"), LevelFluxStatus::NotFitted);

  // A J at an unknown, and at some of the monitors, is not a fitted level.
  save_flux(9, only_j(1.0e-3));
  for (int hole = 1; hole <= 7; ++hole) save_flux(hole, only_j(1.0e-3));
  EXPECT_EQ(level_flux_status(sheet(), "FC-2"), LevelFluxStatus::NotFitted);
  save_flux(8, only_j(1.0e-3));
  EXPECT_EQ(level_flux_status(sheet(), "FC-2"), LevelFluxStatus::Fitted);
}

TEST_P(FluxLevelStatus, FittedAfterASave) {
  const FluxSaveOutcome outcome = save(fitted());
  EXPECT_EQ(outcome.written, 12);
  EXPECT_EQ(level_flux_status(sheet(), "FC-2"), LevelFluxStatus::Fitted);
}

TEST_P(FluxLevelStatus, NoMonitorsWhenNoPositionCarriesTheSample) {
  ASSERT_TRUE(testing::seed_level_without_monitors(store(), seeded(), "B"));
  EXPECT_EQ(level_flux_status(sheet("B"), "FC-2"), LevelFluxStatus::NoMonitors);
  // By the sample asked for: level A has no FCT, and no sample is no monitor.
  EXPECT_EQ(level_flux_status(sheet(), "FCT"), LevelFluxStatus::NoMonitors);
  EXPECT_EQ(level_flux_status(sheet(), ""), LevelFluxStatus::NoMonitors);
  EXPECT_EQ(level_flux_status(ps::LevelSheet{}, "FC-2"), LevelFluxStatus::NoMonitors);
  // The sample decides: by `unk`, B has a monitor, and it has no J.
  EXPECT_EQ(level_flux_status(sheet("B"), "unk"), LevelFluxStatus::NotFitted);
}

// (The fixture hides the struct of the same name in here: the results are `auto`.)
class FluxHeadInfo : public FluxSaveLevel {};

TEST_P(FluxHeadInfo, SaysWhoSavedAHoleAndWhen) {
  // Nothing saved yet: there is no head to tell of.
  auto none = flux_head_info(store(), "NM-300", "A", 7);
  ASSERT_FALSE(none);
  EXPECT_EQ(none.error().kind, ErrorKind::Config);
  EXPECT_EQ(none.error().what, "flux: hole 7 of NM-300A has no saved flux");

  const ps::Uuid first = save_flux(7, only_j(1.0e-3));
  auto info = flux_head_info(store(), "NM-300", "A", 7);
  ASSERT_TRUE(info) << to_string(info.error());
  EXPECT_EQ(info->saved_by, "jsmith");
  // The time of the head's changeset, to the second, without the T and the Z.
  auto history = store().history(*object(7), ps::Kind::RefValue);
  ASSERT_TRUE(history) << to_string(history.error());
  ASSERT_EQ(history->size(), 1u);
  EXPECT_EQ(history->front().uuid, first);
  const std::string iso = history->front().changeset.created.iso();  // YYYY-MM-DDTHH:MM:SS.ffffffZ
  EXPECT_EQ(info->saved_utc, iso.substr(0, 10) + " " + iso.substr(11, 8));
  ASSERT_EQ(info->saved_utc.size(), 19u);
  EXPECT_EQ(info->saved_utc[4], '-');
  EXPECT_EQ(info->saved_utc[10], ' ');
  EXPECT_EQ(info->saved_utc[13], ':');

  // It is the head's author, not the first's: another user saves the hole.
  auto other = store().ensure_user(actor().client, "mlee");
  ASSERT_TRUE(other) << to_string(other.error());
  auto moved = testing::seed_save_flux(store(), ps::Actor{*other, actor().client}, seeded(), 7, only_j(2.0e-3));
  ASSERT_TRUE(moved) << to_string(moved.error());
  info = flux_head_info(store(), "NM-300", "A", 7);
  ASSERT_TRUE(info) << to_string(info.error());
  EXPECT_EQ(info->saved_by, "mlee");
  // Another hole, level and irradiation have their own, or none.
  EXPECT_FALSE(flux_head_info(store(), "NM-300", "A", 8));
  EXPECT_FALSE(flux_head_info(store(), "NM-300", "B", 7));
  EXPECT_FALSE(flux_head_info(store(), "NM-999", "A", 7));

  // The hole a save conflicted on is the one to ask about.
  const LevelFit fit = fitted();  // loaded with mlee's head of hole 7
  save_flux(7, only_j(3.0e-3));
  const FluxSaveOutcome outcome = save(fit);
  ASSERT_TRUE(outcome.conflict);
  EXPECT_EQ(outcome.conflict_hole, 7);
  info = flux_head_info(store(), fit.irradiation, fit.level, outcome.conflict_hole);
  ASSERT_TRUE(info) << to_string(info.error());
  EXPECT_EQ(info->saved_by, "jsmith");
  EXPECT_EQ(FluxSaveOutcome{}.conflict_hole, 0);
}

INSTANTIATE_TEST_SUITE_P(Engines, FluxMonitors, ::testing::ValuesIn(testing::engines()),
                         [](const auto& p) { return p.param; });
INSTANTIATE_TEST_SUITE_P(Engines, FluxLoadLevel, ::testing::ValuesIn(testing::engines()),
                         [](const auto& p) { return p.param; });
INSTANTIATE_TEST_SUITE_P(Engines, FluxSaveLevel, ::testing::ValuesIn(testing::engines()),
                         [](const auto& p) { return p.param; });
INSTANTIATE_TEST_SUITE_P(Engines, FluxSaveSparseLevel, ::testing::ValuesIn(testing::engines()),
                         [](const auto& p) { return p.param; });
INSTANTIATE_TEST_SUITE_P(Engines, FluxLevelStatus, ::testing::ValuesIn(testing::engines()),
                         [](const auto& p) { return p.param; });
INSTANTIATE_TEST_SUITE_P(Engines, FluxHeadInfo, ::testing::ValuesIn(testing::engines()),
                         [](const auto& p) { return p.param; });

}  // namespace
}  // namespace pychron::processing
