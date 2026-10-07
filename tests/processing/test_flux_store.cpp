// Flux monitor sets (flux fitting design, section 4): the defaults, lambda_k,
// the revisioned document (round trip, unknown keys, conflict) and the
// validation of a document. The options JSON of a saved fit (6.4) and
// load_level (6.1) over the seeded level of flux_seed.hpp.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "../reduction/flux_golden.hpp"
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
      {doc("[{\"name\":\"A\",\"age_ma\":28,\"age_err_ma\":0.1,\"lambda_ec\":[5.8e-11,0],\"lambda_b\":[4.9e-10,0]}]"), "sample"},
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
  const std::string json = flux_options_json(o, renne, false, 1.12, 5, "pychron-cpp 0.4.0");
  for (const char* member :
       {R"j("model_kind":"Nearest Neighbors")j", R"j("use_weighted_fit":true)j", R"j("predicted_j_error_type":"sd")j",
        R"j("error_kind":"sem")j", R"j("mean_kind":"weighted")j", R"j("n_neighbors":3)j", R"j("interpolation_style":"Linear")j",
        R"j("one_d_axis":"Y")j", R"j("degree":3)j", R"j("monitor_reference":"FC-2 (Renne 1998)")j",
        R"j("monitor_sample":"FC-2")j", R"j("used_in_fit":false)j", R"j("fit_mswd":1.12)j", R"j("fit_dof":5)j",
        R"j("software":"pychron-cpp 0.4.0")j"})
    EXPECT_TRUE(has(json, member)) << member << " in " << json;

  const FluxOptionsDoc doc = parse_flux_options(json);
  ASSERT_TRUE(doc.options);
  EXPECT_EQ(*doc.options, o);
  EXPECT_EQ(doc.monitor_set, "FC-2 (Renne 1998)");
  EXPECT_EQ(doc.monitor_sample, "FC-2");
  ASSERT_TRUE(doc.used_in_fit.has_value());
  EXPECT_FALSE(*doc.used_in_fit);
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
          const FluxOptionsDoc back = parse_flux_options(flux_options_json(each, renne, true, 0.5, 2, "t"));
          ASSERT_TRUE(back.options) << legacy_model_name(kind);
          EXPECT_EQ(*back.options, each) << legacy_model_name(kind);
          EXPECT_EQ(back.used_in_fit, std::optional<bool>(true));
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
          "predicted_j_error_type":"nonsense","monitor_reference":12,"used_in_fit":"no"})");
  ASSERT_TRUE(odd.options);
  EXPECT_EQ(*odd.options, FluxOptions{});
  EXPECT_EQ(odd.monitor_set, "");
  EXPECT_FALSE(odd.used_in_fit.has_value());
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

// ---- load_level (design section 6.1) ----------------------------------------

class FluxLoadLevel : public testing::FluxStoreTest {
 protected:
  void SetUp() override {
    FluxStoreTest::SetUp();
    if (HasFatalFailure()) return;
    seed_level();
  }

  Result<LevelInputs> load(const MonitorSelection& selection = {}, const std::string& level = "A",
                           const std::string& irradiation = "NM-300") {
    return load_level(source(), store(), irradiation, level, selection);
  }

  static const LevelPosition* hole(const LevelInputs& in, int n) {
    for (const auto& p : in.positions)
      if (p.hole == n) return &p;
    return nullptr;
  }

  static FluxOptions plane_sem() {
    FluxOptions o;
    o.fit.kind = pr::ModelKind::Plane;
    o.fit.error = pr::MeanErrorKind::Sem;
    o.mean_error = pr::MeanErrorKind::Sem;
    return o;
  }

  // A saved J with options naming a monitor set.
  static ps::FluxValue saved_with(const FluxOptions& options, const MonitorSet& set, bool used_in_fit = true) {
    ps::FluxValue v;
    v.j = 1.0e-3;
    v.j_err = 2.0e-7;
    v.options_json = flux_options_json(options, set, used_in_fit, 1.12, 5, "test");
    return v;
  }
};

TEST_F(FluxLoadLevel, PositionsMonitorsAndGeometry) {
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

TEST_F(FluxLoadLevel, GeometryIsByHoleIdNotByIndex) {
  // The same holes listed last first: ordinal 1 is hole "12".
  auto holes = testing::seed_holes();
  std::reverse(holes.begin(), holes.end());
  for (std::size_t i = 0; i < holes.size(); ++i) holes[i].ordinal = static_cast<int>(i + 1);
  ASSERT_EQ(holes.front().hole_id, "12");
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
}

TEST_F(FluxLoadLevel, MonitorSetResolution) {
  const MonitorSets defaults = default_monitor_sets();
  // None named, nothing saved: the document's default.
  auto fresh = load();
  ASSERT_TRUE(fresh) << to_string(fresh.error());
  EXPECT_EQ(fresh->monitor_set.name, "FC-2 (Kuiper 2008)");

  // A saved fit names the set it used.
  save_flux(1, saved_with(plane_sem(), defaults.sets[1]));
  auto saved = load();
  ASSERT_TRUE(saved) << to_string(saved.error());
  EXPECT_EQ(saved->monitor_set, defaults.sets[1]);

  // A set named explicitly wins over the saved fit's.
  MonitorSelection kuiper;
  kuiper.monitor_set = "FC-2 (Kuiper 2008)";
  auto named = load(kuiper);
  ASSERT_TRUE(named) << to_string(named.error());
  EXPECT_EQ(named->monitor_set, defaults.sets[0]);

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
  // "FC Min") falls through to the default.
  ps::FluxValue legacy;
  legacy.j = 1.0e-3;
  legacy.j_err = 2.0e-7;
  legacy.options_json = R"({"model_kind":"Plane","monitor_reference":"FC Min"})";
  save_flux(2, legacy);
  auto fallen = load();
  ASSERT_TRUE(fallen) << to_string(fallen.error());
  EXPECT_EQ(fallen->monitor_set.name, "FC-2 (Kuiper 2008)");
}

TEST_F(FluxLoadLevel, SampleOverrideAndAllPositions) {
  MonitorSelection unk;
  unk.sample = "unk";
  auto in = load(unk);
  ASSERT_TRUE(in) << to_string(in.error());
  EXPECT_EQ(in->monitor_set.name, "FC-2 (Kuiper 2008)");
  EXPECT_EQ(in->monitor_set.sample, "unk");
  ASSERT_EQ(in->positions.size(), 12u);
  for (const auto& p : in->positions) {
    EXPECT_EQ(p.monitor, p.hole >= 9) << p.hole;
    EXPECT_TRUE(p.analyses.empty()) << p.hole;  // 9-12 have none; 1-8 are not monitors
  }

  MonitorSelection all;
  all.all_positions = true;
  auto every = load(all);
  ASSERT_TRUE(every) << to_string(every.error());
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

TEST_F(FluxLoadLevel, ReadsTheSavedFit) {
  FluxOptions options;
  options.fit.kind = pr::ModelKind::Plane;
  options.fit.weighted = true;
  options.fit.error = pr::MeanErrorKind::Sem;
  options.mean = pr::MeanKind::Weighted;
  options.mean_error = pr::MeanErrorKind::Sd;
  ps::FluxValue v = saved_with(options, default_monitor_sets().sets[1], false);
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
  const LevelPosition& p = *hole(*in, 3);
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
  EXPECT_EQ(s.monitor_set, "FC-2 (Renne 1998)");
  EXPECT_EQ(s.omitted, (std::set<std::string>{"66003-02", "66003-03"}));
  EXPECT_EQ(s.saved_by, "jsmith");
  EXPECT_NE(ps::UtcTime::parse(s.saved_utc), std::nullopt) << s.saved_utc;

  const LevelPosition& u = *hole(*in, 10);
  ASSERT_TRUE(u.saved);
  EXPECT_EQ(u.saved->revision, unknown_revision.str());
  EXPECT_EQ(u.saved->j, std::optional<double>(1.0e-3));
  EXPECT_FALSE(u.saved->options);
  for (int h : {1, 2, 4, 9, 12}) EXPECT_FALSE(hole(*in, h)->saved) << h;

  ASSERT_TRUE(in->saved_options);
  EXPECT_EQ(*in->saved_options, options);
  EXPECT_FALSE(in->saved_sd_replaced);
  EXPECT_EQ(in->monitor_set.name, "FC-2 (Renne 1998)");

  // The saved fit shapes the next one: hole 3 stays out and its omissions hold.
  auto fit = fit_level(*in, *in->saved_options, {});
  ASSERT_TRUE(fit) << to_string(fit.error());
  const FittedPosition& fitted = fit->positions[2];
  EXPECT_FALSE(fitted.used_in_fit);
  EXPECT_EQ(fitted.n, 1);
  EXPECT_EQ(fitted.saved_revision, std::optional<std::string>(revision.str()));

  // A newer revision of the position replaces what is read.
  const ps::Uuid newer = save_flux(3, saved_with(plane_sem(), default_monitor_sets().sets[0]));
  auto again = load();
  ASSERT_TRUE(again) << to_string(again.error());
  EXPECT_EQ(hole(*again, 3)->saved->revision, newer.str());
  EXPECT_TRUE(hole(*again, 3)->saved->omitted.empty());
  ASSERT_TRUE(again->saved_options);
  EXPECT_EQ(*again->saved_options, plane_sem());
  EXPECT_EQ(again->monitor_set.name, "FC-2 (Kuiper 2008)");
}

TEST_F(FluxLoadLevel, ASavedRevisionThatIsOnlyAJ) {
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

TEST_F(FluxLoadLevel, TagsAreCarried) {
  tag("66001-02", "omit");
  tag("66002-01", "invalid");  // loaded all the same: a tag omits, it does not hide
  auto in = load();
  ASSERT_TRUE(in) << to_string(in.error());
  const LevelPosition& one = *hole(*in, 1);
  ASSERT_EQ(one.analyses.size(), 3u);
  EXPECT_EQ(one.analyses[0].tag, "ok");
  EXPECT_EQ(one.analyses[1].record_id, "66001-02");
  EXPECT_EQ(one.analyses[1].tag, "omit");
  EXPECT_TRUE(one.analyses[1].f);
  const LevelPosition& two = *hole(*in, 2);
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

TEST_F(FluxLoadLevel, ErrorsNameTheCause) {
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
  auto holes = testing::seed_holes();
  holes.pop_back();
  publish_holder(holes);
  auto no_hole = load();
  ASSERT_FALSE(no_hole);
  EXPECT_EQ(no_hole.error().kind, ErrorKind::Config);
  EXPECT_TRUE(has(no_hole.error().what, "hole 12 is not on holder 12-hole")) << no_hole.error().what;
}

TEST_F(FluxLoadLevel, AnImportedLegacyLevelLoadsAndRefits) {
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
  // "FC Min" is no set of the document: the default.
  EXPECT_EQ(in->monitor_set.name, "FC-2 (Kuiper 2008)");
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
    if (p.monitor) EXPECT_LT(std::abs(*p.dev_percent), 0.5) << p.hole;  // the plane is close to the saved ring J
  }
}

}  // namespace
}  // namespace pychron::processing
