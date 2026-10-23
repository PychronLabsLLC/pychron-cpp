// The arar_reduce unit (domain seam spec 3.2): after core's reduce, it reduces
// the argon isotopes from the signals core minted, so its stages share their
// variables with core's, and it carries the Ar-Ar result as the domain's part
// of the reduced analysis.

#include "pychron/arar/reduce.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "arar_fixtures.hpp"
#include "pychron/arar/domain.hpp"
#include "pychron/processing/domain.hpp"
#include "pychron/processing/quantity.hpp"
#include "pychron/processing/units.hpp"
#include "reduce_golden.hpp"

namespace pp = pychron::processing;
namespace r = pychron::reduction;
namespace g = pychron::reduction::golden;
using pychron::arar::edit_analysis_data;

namespace {

// A memory source with an age cache that records what it is given.
struct SinkSource : pp::MemorySource, pp::IDerivedSink {
  pp::IDerivedSink* derived() noexcept override { return this; }
  void put(std::vector<pp::DerivedValues> values) override { puts.push_back(std::move(values)); }
  std::vector<std::vector<pp::DerivedValues>> puts;
};

// select, then the domain's reduction chain.
pp::Pipeline reduce_pipeline(const pp::Domain& domain) {
  pp::Pipeline p;
  const auto& reg = domain.units();
  p.add(reg, "select", "select");
  std::string previous = "select";
  for (const auto& kind : domain.reduction_chain()) {
    p.add(reg, kind, kind, {previous});
    previous = kind;
  }
  return p;
}

struct Ran {
  pp::DatasetPtr dataset;
  std::vector<std::string> diagnostics;
};

Ran run_chain(pp::IAnalysisSource& source, const pp::Domain& domain) {
  pp::Runner runner(domain.units(), &source, &domain);
  const auto pipeline = reduce_pipeline(domain);
  auto ran = runner.run(pipeline, domain.reduction_chain().back());
  EXPECT_TRUE(ran) << (ran ? std::string() : ran.error().what);
  if (!ran) return {};
  return {std::get<pp::DatasetPtr>(ran->at(0)), runner.diagnostics()};
}

pp::Value value_of(const g::Json& j) { return {j["v"].as_number(), j["e"].as_number()}; }

// One golden pipeline case as an analysis the reduce unit would load: its
// signals, its constants (which win over the preset), its production ratios,
// its J, and its irradiation segments as doses ending before the run.
std::shared_ptr<pp::Analysis> analysis_of(const g::Json& in, const r::ReductionConstants& rc, const std::string& uuid) {
  auto a = std::make_shared<pp::Analysis>();
  a->uuid = uuid;
  a->identifier = "G";
  a->runid = uuid;
  a->timestamp = 1'700'000'000.0;
  bool blank_corrected = true;
  for (const r::ArgonIsotope iso : r::kArgonKeys) {
    const std::string key(r::to_string(iso));
    const g::Json& j = in["isotopes"][key];
    pp::IsotopeData d;
    d.key = key;
    d.isotope = key;
    d.intercept = value_of(j["intercept"]);
    d.baseline = value_of(j["baseline"]);
    d.blank = value_of(j["blank"]);
    d.ic_factor = value_of(j["ic_factor"]);
    d.include_baseline_error = j["include_baseline_error"].as_bool();
    blank_corrected = j["correct_for_blank"].as_bool();
    a->isotopes.push_back(d);
  }
  // No production ratios or chronology needed for an age: an air is not irradiated.
  a->analysis_type = blank_corrected ? "air" : "blank_unknown";
  edit_analysis_data(*a).constants = rc;
  std::map<std::string, r::Measured, std::less<>> rows;
  for (const auto& [key, m] : in["production"].as_object()) rows[key] = g::measured_of(m);
  auto production = r::production_from_rows(rows);
  EXPECT_TRUE(production);
  if (production) edit_analysis_data(*a).production = *production;
  const auto t = static_cast<std::int64_t>(a->timestamp);
  for (const g::Json& s : in["irradiation"]["segments"].as_array()) {
    const auto dt = static_cast<std::int64_t>(s["dt_days"].as_number() * 86'400.0);
    const auto duration = static_cast<std::int64_t>(s["duration_days"].as_number() * 86'400.0);
    const std::int64_t ref = t - dt;
    const std::int64_t start = rc.use_irradiation_endtime ? ref - duration : ref;
    edit_analysis_data(*a).chronology.push_back(r::Dose{s["power"].as_number(), start, start + duration});
  }
  if (!in["j"].is_null()) {
    r::Flux flux{g::measured_of(in["j"]), in["position_jerr"].as_number(), std::nullopt};
    if (!in["lambda_k_total"].is_null()) flux.lambda_k_total = g::measured_of(in["lambda_k_total"]);
    edit_analysis_data(*a).flux = flux;
  }
  if (!in["fixed_k3739"].is_null()) edit_analysis_data(*a).fixed_k3739 = g::measured_of(in["fixed_k3739"]);
  return a;
}

// make_unknown(1) with real decay and production ratios (Phase 1's reference
// values, variant B).
std::shared_ptr<pp::Analysis> decayed_unknown() {
  auto a = pychron::arar::test::make_unknown(1);
  const auto t = static_cast<std::int64_t>(a->timestamp);
  edit_analysis_data(*a).chronology = {r::Dose{1.0, t - 365 * 86'400, t - 365 * 86'400 + 36'000}};
  r::ProductionRatios p;
  p.k4039 = {0.0008, 5e-5};
  p.ca3937 = {0.0007, 1e-5};
  p.ca3637 = {0.00027, 2e-6};
  edit_analysis_data(*a).production = p;
  return a;
}

}  // namespace

// The golden pipeline cases, loaded as analyses and run through select, reduce
// and arar_reduce, give the golden ages to the golden tolerances. The chlorine
// cases are left out: their decay days are no doses' (a dose chronology says
// when the irradiation began; the golden case gives a number of its own).
TEST(ArArReduceUnit, MatchesTheGoldenReduce) {
  const g::Json doc = g::load("pipeline.json");
  pp::MemorySource source;
  std::map<std::string, const g::Json*> cases;
  int n = 0;
  for (const g::Json& c : doc["cases"].as_array()) {
    const std::string name = c["name"].string;
    if (g::is_chlorine(name) || !c["expect_error"].is_null()) continue;
    if (!c["expected"].contains("ages")) continue;
    const g::Json& in = c["inputs"];
    r::ReductionConstants rc;
    if (!g::constants_of(in["constants"], rc, name)) continue;
    const std::string uuid = "golden-" + std::to_string(n++);
    source.add(analysis_of(in, rc, uuid));
    cases[uuid] = &c;
  }
  ASSERT_EQ(cases.size(), 42u);

  const auto domain = pychron::arar::make_domain();
  const Ran ran = run_chain(source, *domain);
  ASSERT_TRUE(ran.dataset);
  ASSERT_EQ(ran.dataset->items().size(), cases.size());
  for (const auto& item : ran.dataset->items()) {
    const g::Json& c = *cases.at(item.analysis->analysis->uuid);
    SCOPED_TRACE(c["name"].string);
    const auto* arar = pychron::arar::reduced_of(*item.analysis);
    ASSERT_NE(arar, nullptr) << item.analysis->reduction_error;
    ASSERT_TRUE(arar->arar.ages);
    const g::Tol t = g::tol_of(c);
    g::check_u(arar->arar.ages->age, c["expected"]["ages"]["age"], t, "ages.age");
  }
}

// A ratio of an Ar-Ar stage to a core stage of the same isotope carries the
// shared variables: the decay factor alone is left, with no error.
TEST(ArArReduceUnit, StagesShareVariablesWithTheCoreStages) {
  pp::MemorySource source;
  source.add(decayed_unknown());
  const auto domain = pychron::arar::make_domain();
  const Ran ran = run_chain(source, *domain);
  ASSERT_TRUE(ran.dataset);
  ASSERT_EQ(ran.dataset->items().size(), 1u);
  const pp::ReducedAnalysis& ra = *ran.dataset->items()[0].analysis;
  ASSERT_NE(ra.domain, nullptr) << ra.reduction_error;

  const auto ic39 = ra.stage("Ar39", pp::Stage::IcCorrected);
  const auto dc39 = ra.domain->stage("Ar39", "decay_corrected");
  ASSERT_TRUE(ic39 && dc39);
  const r::UFloat decay = *dc39 / *ic39;
  EXPECT_NEAR(decay.nominal(), 1.0025846269193437, 1e-15);
  EXPECT_EQ(decay.std_dev(), 0.0);

  const auto ic40 = ra.stage("Ar40", pp::Stage::IcCorrected);
  const auto if40 = ra.domain->stage("Ar40", "interference_corrected");
  ASSERT_TRUE(ic40 && if40);
  EXPECT_NEAR((*if40 / *ic40).std_dev(), 4.9900160673479243e-06, 1e-15);

  const auto age = ra.domain->named("age");
  ASSERT_TRUE(age);
  EXPECT_NEAR(age->nominal(), 16.328191319278158, 1e-12);
  EXPECT_NEAR(age->std_dev(), 0.060912656006389537, 1e-12);

  EXPECT_EQ(ra.domain->domain(), "arar");
  EXPECT_EQ(ra.domain->stages(), (std::vector<std::string>{"decay_corrected", "interference_corrected"}));
  EXPECT_FALSE(ra.domain->stage("Ar39", "ic_corrected"));
  EXPECT_FALSE(ra.domain->named("no_such_value"));
}

// No argon, no Ar-Ar result: the analysis keeps its core stages and says why.
TEST(ArArReduceUnit, AHeliumOnlyAnalysisDoesNotReduce) {
  auto a = std::make_shared<pp::Analysis>();
  a->uuid = "he-1";
  a->identifier = "H";
  a->runid = "H-01";
  a->analysis_type = "unknown";
  a->timestamp = 1'700'000'000.0;
  for (const char* name : {"He3", "He4"}) {
    pp::IsotopeData d;
    d.key = name;
    d.isotope = name;
    d.intercept = {10.0, 0.1};
    d.baseline = {0.01, 0.001};
    a->isotopes.push_back(d);
  }
  pp::MemorySource source;
  source.add(a);
  const auto domain = pychron::arar::make_domain();
  const Ran ran = run_chain(source, *domain);
  ASSERT_TRUE(ran.dataset);
  ASSERT_EQ(ran.dataset->items().size(), 1u);
  const pp::ReducedAnalysis& ra = *ran.dataset->items()[0].analysis;
  EXPECT_FALSE(ra.reduction_error.empty());
  EXPECT_EQ(ra.domain, nullptr);
  EXPECT_EQ(pychron::arar::reduced_of(ra), nullptr);
  EXPECT_EQ(ra.isotope_signals.size(), 2u);
  EXPECT_TRUE(ra.stage("He4", pp::Stage::IcCorrected));
  // The unit says "1 analyses did not reduce"; the runner names the node.
  EXPECT_EQ(ran.diagnostics, std::vector<std::string>{"arar_reduce: 1 analyses did not reduce"});
}

// The unit that makes the derived values writes them, once per run, under the
// domain's reduction version; core's reduce writes nothing.
TEST(ArArReduceUnit, WritesTheCacheOnceWithTheArArVersion) {
  SinkSource source;
  for (int i = 0; i < 2; ++i) {
    auto a = pychron::arar::test::make_unknown(i);
    a->input_fingerprint = std::string(64, 'a');
    source.add(a);
  }
  const auto domain = pychron::arar::make_domain();
  pp::Runner runner(domain->units(), &source, domain.get());
  const auto pipeline = reduce_pipeline(*domain);

  auto core = runner.run(pipeline, "reduce");
  ASSERT_TRUE(core) << core.error().what;
  EXPECT_TRUE(source.puts.empty());

  auto ran = runner.run(pipeline, "arar_reduce");
  ASSERT_TRUE(ran) << ran.error().what;
  ASSERT_EQ(source.puts.size(), 1u);
  ASSERT_EQ(source.puts[0].size(), 2u);
  for (const auto& d : source.puts[0]) {
    std::vector<std::string> names;
    names.reserve(d.values.size());
    for (const auto& v : d.values) names.push_back(v.name);
    EXPECT_EQ(names, (std::vector<std::string>{"age", "age_w_j", "kca"}));
  }
  const auto dataset = std::get<pp::DatasetPtr>(ran->at(0));
  EXPECT_TRUE(dataset->reduction_tag.starts_with("arar-1/")) << dataset->reduction_tag;
  EXPECT_EQ(dataset->reduction_tag, "arar-1/default/d0e0");

  // Another reduction setting is no cached one.
  source.puts.clear();
  pp::Pipeline other = reduce_pipeline(*domain);
  ASSERT_TRUE(other.find("arar_reduce")->options.set("include_decay_error", true));
  ASSERT_TRUE(runner.run(other, "arar_reduce"));
  EXPECT_TRUE(source.puts.empty());
}

// The j quantity is a J of its own, as it was before the domain seam: a ratio
// of an age to it does not cancel the age's J, which ArArReduced::j (the
// variable the ages used, for group ages) would.
TEST(ArArReduceUnit, TheJQuantityIsAnIndependentVariable) {
  pp::MemorySource source;
  source.add(decayed_unknown());
  const auto domain = pychron::arar::make_domain();
  const Ran ran = run_chain(source, *domain);
  ASSERT_TRUE(ran.dataset);
  ASSERT_EQ(ran.dataset->items().size(), 1u);
  const pp::ReducedAnalysis& ra = *ran.dataset->items()[0].analysis;
  const auto* arar = pychron::arar::reduced_of(ra);
  ASSERT_NE(arar, nullptr) << ra.reduction_error;
  ASSERT_TRUE(arar->j && arar->arar.ages);

  // `age` carries J without its error, so it shares no J variable either way;
  // `age_w_j` does, and tells the two apart.
  const auto q = pp::Quantity::parse("age_w_j/j");
  ASSERT_TRUE(q) << q.error().what;
  const auto ratio = q->eval(ra);
  ASSERT_TRUE(ratio);
  const r::UFloat& age = arar->arar.ages->age_w_j_err;
  const r::UFloat independent = age / r::UFloat::variable(arar->j->nominal(), arar->j->std_dev(), "J");
  const r::UFloat correlated = age / *arar->j;
  EXPECT_EQ(ratio->value, independent.nominal());
  EXPECT_NEAR(ratio->error, independent.std_dev(), 1e-15 * independent.std_dev());
  EXPECT_GT(std::abs(ratio->error - correlated.std_dev()), 1e-3 * independent.std_dev());

  // age/j: the old formula as well.
  const auto plain = pp::Quantity::parse("age/j")->eval(ra);
  ASSERT_TRUE(plain);
  const r::UFloat old = arar->arar.ages->age / r::UFloat::variable(arar->j->nominal(), arar->j->std_dev(), "J");
  EXPECT_NEAR(plain->error, old.std_dev(), 1e-15 * old.std_dev());

  // And `j` alone is the J the ages used, in value and error.
  const auto j = pp::Quantity::parse("j")->eval(ra);
  ASSERT_TRUE(j);
  EXPECT_EQ(j->value, arar->j->nominal());
  EXPECT_EQ(j->error, arar->j->std_dev());
}
