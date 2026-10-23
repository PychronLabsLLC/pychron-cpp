// The Ar-Ar extension loader of the store source (domain seam spec 3.3):
// the flux, production ratios and chronology of an analysis from its resolved
// references, as StoreSource read them before the seam, and what rests on
// them: the input fingerprint, the age cache the Ar-Ar reduction fills, the
// backfill, and the mappings of stored numbers into the reduction. The store
// is tests/processing store_source_fixture.hpp's; the source here opens with
// arar::store_source_options().

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "pychron/arar/analysis_data.hpp"
#include "pychron/arar/derived.hpp"
#include "pychron/arar/domain.hpp"
#include "pychron/arar/reduce.hpp"
#include "pychron/arar/store_loader.hpp"
#include "pychron/core/sha256.hpp"
#include "pychron/processing/derive.hpp"
#include "pychron/processing/quantity.hpp"
#include "pychron/processing/recall.hpp"
#include "pychron/processing/reduced.hpp"
#include "pychron/processing/store_source.hpp"
#include "pychron/processing/units.hpp"
#include "store_source_fixture.hpp"

namespace pychron::processing {
namespace {

namespace ps = pychron::persistence;
using test::argon_parts;
using test::first_row;

// The Ar-Ar part of a reduction made by arar::reduce_with_arar, or null.
const arar::ArArReduced* ar_of(const ReducedPtr& reduced) { return reduced ? arar::reduced_of(*reduced) : nullptr; }

// The Ar-Ar data the loader gave `a`: every analysis it loads has some.
const arar::ArArAnalysisData& refs_of(const Analysis& a) {
  static const arar::ArArAnalysisData none;
  const arar::ArArAnalysisData* data = arar::analysis_data_of(a);
  EXPECT_NE(data, nullptr) << a.runid << " has no Ar-Ar data";
  return data != nullptr ? *data : none;
}

// The value of `name` in `d`, when it has one.
std::optional<reduction::UFloat> dv(const DerivedValues& d, std::string_view name) {
  const DerivedValue* v = find_derived(d, name);
  return v ? v->value : std::nullopt;
}

// The seeded store, read with the Ar-Ar loader.
class StoreSourceTest : public test::StoreSourceFixture {
 protected:
  [[nodiscard]] StoreSourceOptions source_options() const override {
    return arar::store_source_options(StoreSourceFixture::source_options());
  }

  // What a reduction of `uuid` with the default settings gives the cache.
  DerivedValues derived_of(const ps::Uuid& uuid) {
    auto loaded = source().load(uuid.str());
    EXPECT_TRUE(loaded) << (loaded ? "" : to_string(loaded.error()));
    if (!loaded) return {};
    auto d = derived_age(*arar::reduce_with_arar(*loaded));
    EXPECT_TRUE(d);
    return d ? *d : DerivedValues{};
  }

  // The cached rows of `uuid` by name, when they are of its current inputs.
  std::map<std::string, ps::DerivedRow> cached(const ps::Uuid& uuid) {
    std::map<std::string, ps::DerivedRow> out;
    auto rows = store_->get_derived(uuid, std::string(arar::shared_domain().reduction_version()));
    EXPECT_TRUE(rows) << (rows ? "" : to_string(rows.error()));
    if (rows && *rows)
      for (const auto& r : **rows) out[r.name] = r;
    return out;
  }
};

TEST_F(StoreSourceTest, ReferenceValuesWithoutContentAreNoValues) {
  // What an import leaves at the head of reference data the source removed:
  // a value with nothing in it. It must not be read as numbers.
  publish(ref_object(ps::RefType::FluxPosition, "NM-300/A/3", {}), ps::FluxValue{});
  publish(ref_object(ps::RefType::Production, "NM-300/Triga", {}), ps::ProductionValue{});
  publish(ref_object(ps::RefType::Chronology, "NM-300", {}), ps::ChronologyValue{});
  auto loaded = source().load(unknown_.str());
  ASSERT_TRUE(loaded) << to_string(loaded.error());
  EXPECT_FALSE(refs_of(**loaded).flux);
  EXPECT_FALSE(refs_of(**loaded).production);  // not nine ratios of zero
  EXPECT_TRUE(refs_of(**loaded).chronology.empty());
  const auto reduced = arar::reduce_with_arar(*loaded);
  EXPECT_FALSE(ar_of(reduced) && ar_of(reduced)->arar.ages);
}

// ---- The sink (age cache spec 4.1, 4.4, 4.5) ---------------------------------

TEST_F(StoreSourceTest, TheSinkFillsTheCache) {
  auto& src = source();
  ASSERT_NE(src.derived(), nullptr);
  EXPECT_TRUE(cached(unknown_).empty());
  const DerivedValues d = derived_of(unknown_);
  ASSERT_TRUE(dv(d, "age") && dv(d, "age_w_j") && dv(d, "kca"));

  src.derived()->put({d});
  const auto done = src.flush_derived();
  EXPECT_EQ(done.written, 1u);
  EXPECT_EQ(done.current, 0u);
  EXPECT_EQ(done.failed, 0u) << done.last_error;

  const auto rows = cached(unknown_);
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_DOUBLE_EQ(*rows.at("age").value, dv(d, "age")->nominal());
  EXPECT_DOUBLE_EQ(*rows.at("age").error, dv(d, "age")->std_dev());
  EXPECT_EQ(rows.at("age").units, "Ma");
  EXPECT_DOUBLE_EQ(*rows.at("age_w_j").value, dv(d, "age_w_j")->nominal());
  EXPECT_DOUBLE_EQ(*rows.at("age_w_j").error, dv(d, "age_w_j")->std_dev());
  EXPECT_EQ(rows.at("age_w_j").units, "Ma");
  EXPECT_DOUBLE_EQ(*rows.at("kca").value, dv(d, "kca")->nominal());
  EXPECT_DOUBLE_EQ(*rows.at("kca").error, dv(d, "kca")->std_dev());
  EXPECT_FALSE(rows.at("kca").units);
  // nothing was queued since: a flush has nothing to say
  const auto idle = src.flush_derived();
  EXPECT_EQ(idle.written + idle.current + idle.failed, 0u);
}

// What the sink wrote is what browse shows, and what an age range finds.
TEST_F(StoreSourceTest, ASummaryCarriesTheAgeTheSinkWrote) {
  auto& src = source();
  const DerivedValues d = derived_of(unknown_);
  ASSERT_TRUE(dv(d, "age"));
  src.derived()->put({d, derived_of(air_)});
  ASSERT_EQ(src.flush_derived().failed, 0u);

  auto page = src.browse(BrowseQuery{});
  ASSERT_TRUE(page) << to_string(page.error());
  auto summary = [&](const ps::Uuid& u) -> const AnalysisSummary* {
    for (const auto& r : page->rows)
      if (r.uuid == u.str()) return &r;
    return nullptr;
  };
  const AnalysisSummary* unknown = summary(unknown_);
  ASSERT_NE(unknown, nullptr);
  ASSERT_TRUE(unknown->age);
  EXPECT_DOUBLE_EQ(*unknown->age, dv(d, "age")->nominal());
  ASSERT_TRUE(unknown->age_err);
  EXPECT_DOUBLE_EQ(*unknown->age_err, dv(d, "age")->std_dev());
  const AnalysisSummary* air = summary(air_);
  ASSERT_NE(air, nullptr);
  EXPECT_FALSE(air->age) << "an air is cached as having no age";

  BrowseQuery around;
  around.age_min = dv(d, "age")->nominal() - 1e-6;
  around.age_max = dv(d, "age")->nominal() + 1e-6;
  auto found = src.browse(around);
  ASSERT_TRUE(found) << to_string(found.error());
  ASSERT_EQ(found->rows.size(), 1u);
  EXPECT_EQ(found->rows[0].uuid, unknown_.str());
  EXPECT_EQ(found->total, 1u);
  auto types = src.facet(Facet::AnalysisType, around);
  ASSERT_TRUE(types) << to_string(types.error());
  EXPECT_EQ(*types, (std::vector<std::string>{"unknown"}));
  BrowseQuery above;
  above.age_min = dv(d, "age")->nominal() + 1.0;
  auto none = src.browse(above);
  ASSERT_TRUE(none) << to_string(none.error());
  EXPECT_TRUE(none->rows.empty());
}

// Opening a figure of analyses whose ages are current costs one read and no write.
TEST_F(StoreSourceTest, ACurrentAgeIsNotWrittenAgain) {
  auto& src = source();
  const DerivedValues unknown = derived_of(unknown_);
  const DerivedValues air = derived_of(air_);
  src.derived()->put({unknown});
  EXPECT_EQ(src.flush_derived().written, 1u);

  src.derived()->put({unknown});
  auto done = src.flush_derived();
  EXPECT_EQ(done.written, 0u);
  EXPECT_EQ(done.current, 1u);

  // a batch of one current and one new: only the new one is written
  src.derived()->put({unknown, air});
  done = src.flush_derived();
  EXPECT_EQ(done.written, 1u);
  EXPECT_EQ(done.current, 1u);
  EXPECT_EQ(done.failed, 0u) << done.last_error;
  // an analysis given twice in one batch is one write
  auto third = ingest("77000", 2, "unknown", "2026-10-02T12:00:00Z");
  const DerivedValues d3 = derived_of(third);
  src.derived()->put({d3, d3});
  done = src.flush_derived();
  EXPECT_EQ(done.written, 1u);
  EXPECT_EQ(done.failed, 0u) << done.last_error;
  EXPECT_EQ(cached(third).size(), 3u);
  // and an empty batch is nothing at all
  src.derived()->put({});
  done = src.flush_derived();
  EXPECT_EQ(done.written + done.current + done.failed, 0u);
}

TEST_F(StoreSourceTest, AChangedFluxReplacesTheAge) {
  auto& src = source();
  const DerivedValues before = derived_of(unknown_);
  src.derived()->put({before});
  ASSERT_EQ(src.flush_derived().written, 1u);
  ASSERT_EQ(cached(unknown_).size(), 3u);

  ps::FluxValue flux;
  flux.j = 0.002;
  flux.j_err = 2e-6;
  flux.position_jerr = 2e-7;
  flux.monitor_name = "FC-2";
  flux.monitor_material = "sanidine";
  flux.monitor_age = 28.201;
  flux.monitor_age_err = 0.023;
  publish(flux_object_, flux);
  // The age cached is of other inputs now: it is not served.
  EXPECT_TRUE(cached(unknown_).empty());

  ASSERT_TRUE(src.refresh());
  const DerivedValues after = derived_of(unknown_);
  ASSERT_NE(after.input_fingerprint, before.input_fingerprint);
  ASSERT_TRUE(dv(after, "age"));
  EXPECT_GT(dv(after, "age")->nominal(), 1.5 * dv(before, "age")->nominal());
  src.derived()->put({after});
  const auto done = src.flush_derived();
  EXPECT_EQ(done.written, 1u);
  EXPECT_EQ(done.failed, 0u) << done.last_error;
  const auto rows = cached(unknown_);
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_DOUBLE_EQ(*rows.at("age").value, dv(after, "age")->nominal());
  // one fingerprint an analysis: the old rows are gone, not beside the new
  const auto fingerprints = store_->derived_fingerprints(std::vector<ps::Uuid>{unknown_});
  ASSERT_TRUE(fingerprints);
  EXPECT_EQ(to_hex(fingerprints->at(unknown_)), after.input_fingerprint);

  // An age of inputs that are no longer the analysis's is still what that
  // reduction gave: it is written, and not served as current.
  src.derived()->put({before});
  EXPECT_EQ(src.flush_derived().written, 1u);
  EXPECT_TRUE(cached(unknown_).empty());
}

TEST_F(StoreSourceTest, AnAnalysisWithNoAgeGetsANullAge) {
  auto& src = source();
  const DerivedValues air = derived_of(air_);
  ASSERT_FALSE(dv(air, "age"));
  src.derived()->put({air});
  const auto done = src.flush_derived();
  EXPECT_EQ(done.written, 1u);
  EXPECT_EQ(done.failed, 0u) << done.last_error;
  const auto rows = cached(air_);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_FALSE(rows.at("age").value);
  EXPECT_FALSE(rows.at("age").error);
  // "current, and there is no age": not written again
  src.derived()->put({air});
  EXPECT_EQ(src.flush_derived().current, 1u);
}

// A cache: an age with a malformed id or fingerprint is counted and dropped,
// and so is a whole batch the store refuses (it is one transaction).
TEST_F(StoreSourceTest, WhatTheSinkCannotWriteIsDroppedAndCounted) {
  auto& src = source();
  DerivedValues no_such = derived_of(unknown_);
  no_such.uuid = ps::Uuid::v7().str();  // no analysis of the store
  DerivedValues bad_uuid = derived_of(unknown_);
  bad_uuid.uuid = "not a uuid";
  DerivedValues bad_fingerprint = derived_of(unknown_);
  bad_fingerprint.input_fingerprint = "zz";
  src.derived()->put({bad_uuid, bad_fingerprint});
  auto done = src.flush_derived();
  EXPECT_EQ(done.written, 0u);
  EXPECT_EQ(done.failed, 2u);
  EXPECT_FALSE(done.last_error.empty());
  EXPECT_TRUE(cached(unknown_).empty());

  src.derived()->put({no_such});  // the store refuses it: a row for no analysis
  done = src.flush_derived();
  EXPECT_EQ(done.written, 0u);
  EXPECT_EQ(done.failed, 1u);
  EXPECT_FALSE(done.last_error.empty());

  // The source still works, and writes are still taken.
  src.derived()->put({derived_of(unknown_)});
  EXPECT_EQ(src.flush_derived().written, 1u);
}

// ---- Every reduction the application makes writes the cache (spec 4.5) -----

namespace {

// The Ar-Ar domain's units, for the age cache's tests: the cache is the domain's.
const Domain& arar_units_domain() {
  static const std::unique_ptr<Domain> domain = arar::make_domain();
  return *domain;
}

// select, reduce, arar_reduce: what writes the age cache.
Pipeline reduce_pipeline(const std::vector<std::string>& uuids, const std::string& constants = "default") {
  const auto& reg = arar_units_domain().units();
  Pipeline p;
  auto& select = p.add(reg, "select", "select");
  EXPECT_TRUE(select.options.set("uuids", uuids));
  p.add(reg, "reduce", "reduce", {"select"});
  auto& reduce = p.add(reg, "arar_reduce", "arar_reduce", {"reduce"});
  EXPECT_TRUE(reduce.options.set("constants", constants));
  return p;
}

}  // namespace

TEST_F(StoreSourceTest, ReducingThroughAUnitFillsTheCache) {
  auto& src = source();
  const Domain* domain = &arar_units_domain();
  Runner runner(domain->units(), &src, domain);
  auto out = runner.run(reduce_pipeline({unknown_.str(), air_.str()}), "arar_reduce");
  ASSERT_TRUE(out) << to_string(out.error());
  const auto done = src.flush_derived();
  EXPECT_EQ(done.written, 2u);
  EXPECT_EQ(done.failed, 0u) << done.last_error;
  const DerivedValues d = derived_of(unknown_);
  const auto rows = cached(unknown_);
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_DOUBLE_EQ(*rows.at("age").value, dv(d, "age")->nominal());
  EXPECT_EQ(cached(air_).size(), 1u);  // an air: a null age

  // The same figure again on a fresh runner: one read, no write.
  Runner again(domain->units(), &src, domain);
  ASSERT_TRUE(again.run(reduce_pipeline({unknown_.str(), air_.str()}), "arar_reduce"));
  const auto second = src.flush_derived();
  EXPECT_EQ(second.written, 0u);
  EXPECT_EQ(second.current, 2u);
}

TEST_F(StoreSourceTest, AUnitWithOtherSettingsWritesNothing) {
  auto& src = source();
  const auto other = [&](const char* key, OptionValue value) {
    auto p = reduce_pipeline({unknown_.str()});
    EXPECT_TRUE(p.find("arar_reduce")->options.set(key, std::move(value)));
    return p;
  };
  const Domain* domain = &arar_units_domain();
  for (const auto& p : {reduce_pipeline({unknown_.str()}, "legacy"), other("include_decay_error", true),
                        other("use_irradiation_endtime", true)}) {
    Runner runner(domain->units(), &src, domain);
    ASSERT_TRUE(runner.run(p, "arar_reduce"));
    const auto done = src.flush_derived();
    EXPECT_EQ(done.written + done.current + done.failed, 0u);
    EXPECT_TRUE(cached(unknown_).empty());
  }
}

// Queued writes are done before the source closes: nothing is lost by
// closing a window right after a figure was drawn.
TEST_F(StoreSourceTest, ClosingTheSourceFinishesTheQueuedWrites) {
  const DerivedValues unknown = derived_of(unknown_);
  const DerivedValues air = derived_of(air_);
  for (int i = 0; i < 20; ++i) source().derived()->put({unknown, air});
  source_.reset();
  EXPECT_EQ(cached(unknown_).size(), 3u);
  EXPECT_EQ(cached(air_).size(), 1u);
}

// ---------------------------------------------------------------- backfill
// derive_ages (age cache spec 4.5, writer 2), on the seeded store: an
// irradiated unknown that has an age and an air that has none.

bool always(const DeriveStats&) { return true; }

// The domain whose derive pass the backfill runs.
const Domain& arar_domain() {
  static const std::unique_ptr<Domain> domain = arar::make_domain();
  return *domain;
}

TEST_F(StoreSourceTest, TheBackfillFillsAnEmptyCache) {
  auto stats = derive_ages(source(), arar_domain(), {}, always);
  ASSERT_TRUE(stats) << to_string(stats.error());
  EXPECT_EQ(stats->seen, 2);
  EXPECT_EQ(stats->written, 2);
  EXPECT_EQ(stats->no_age, 1);
  EXPECT_EQ(stats->current, 0);
  EXPECT_EQ(stats->failed, 0);
  const auto rows = cached(unknown_);
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_DOUBLE_EQ(*rows.at("age").value, dv(derived_of(unknown_), "age")->nominal());
  ASSERT_EQ(cached(air_).size(), 1u);
  EXPECT_FALSE(cached(air_).at("age").value);
}

TEST_F(StoreSourceTest, ASecondBackfillFindsEverythingCurrent) {
  ASSERT_TRUE(derive_ages(source(), arar_domain(), {}, always));
  auto again = derive_ages(source(), arar_domain(), {}, always);
  ASSERT_TRUE(again) << to_string(again.error());
  EXPECT_EQ(again->seen, 2);
  EXPECT_EQ(again->current, 2);  // the air's null age too
  EXPECT_EQ(again->written, 0);
}

TEST_F(StoreSourceTest, AfterAFluxChangeTheBackfillWritesOnlyWhatItChanged) {
  ASSERT_TRUE(derive_ages(source(), arar_domain(), {}, always));
  ps::FluxValue flux;
  flux.j = 0.002;
  flux.j_err = 2e-6;
  flux.position_jerr = 2e-7;
  flux.monitor_name = "FC-2";
  flux.monitor_material = "sanidine";
  flux.monitor_age = 28.201;
  flux.monitor_age_err = 0.023;
  publish(flux_object_, flux);
  ASSERT_TRUE(source().refresh());
  auto stats = derive_ages(source(), arar_domain(), {}, always);
  ASSERT_TRUE(stats) << to_string(stats.error());
  EXPECT_EQ(stats->written, 1);  // the unknown of that position
  EXPECT_EQ(stats->current, 1);  // the air
  const auto rows = cached(unknown_);
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_DOUBLE_EQ(*rows.at("age").value, dv(derived_of(unknown_), "age")->nominal());
}

TEST_F(StoreSourceTest, AllWritesWhatIsCurrentToo) {
  ASSERT_TRUE(derive_ages(source(), arar_domain(), {}, always));
  DeriveOptions all;
  all.all = true;
  auto stats = derive_ages(source(), arar_domain(), all, always);
  ASSERT_TRUE(stats) << to_string(stats.error());
  EXPECT_EQ(stats->written, 2);
  EXPECT_EQ(stats->current, 0);
}

TEST_F(StoreSourceTest, TheBackfillWalksNoFurtherThanItsLimit) {
  DeriveOptions one;
  one.limit = 1;
  auto stats = derive_ages(source(), arar_domain(), one, always);
  ASSERT_TRUE(stats) << to_string(stats.error());
  EXPECT_EQ(stats->seen, 1);
  EXPECT_EQ(cached(air_).size(), 1u);  // the newest
  EXPECT_TRUE(cached(unknown_).empty());
}

TEST_F(StoreSourceTest, TheBackfillWalksInvalidAnalysesToo) {
  auto uow = *store_->begin(reducer());
  ASSERT_TRUE(uow->add_revision(unknown_, ps::Kind::Tags, ps::TagValue{"invalid", std::nullopt, std::nullopt},
                                *store_->head(unknown_, ps::Kind::Tags)));
  ASSERT_TRUE(uow->commit(ps::ChangesetKind::Reduction, "tag"));
  auto stats = derive_ages(source(), arar_domain(), {}, always);
  ASSERT_TRUE(stats) << to_string(stats.error());
  EXPECT_EQ(stats->seen, 2);
  EXPECT_EQ(cached(unknown_).size(), 3u);
}

TEST_F(StoreSourceTest, TheBackfillWalksPageAfterPage) {
  (void)ingest("77000", 2, "unknown", "2026-10-02T12:00:00Z");
  DeriveOptions pages;
  pages.page_size = 1;
  std::vector<int> seen_after;
  auto stats = derive_ages(source(), arar_domain(), pages, [&](const DeriveStats& so_far) {
    seen_after.push_back(so_far.seen);
    return true;
  });
  ASSERT_TRUE(stats) << to_string(stats.error());
  EXPECT_EQ(seen_after, (std::vector<int>{1, 2, 3}));  // every analysis once, none twice
  EXPECT_EQ(stats->written, 3);
  // and with a limit across pages
  DeriveOptions two = pages;
  two.limit = 2;
  two.all = true;
  auto limited = derive_ages(source(), arar_domain(), two, always);
  ASSERT_TRUE(limited);
  EXPECT_EQ(limited->seen, 2);
}

TEST_F(StoreSourceTest, TheBackfillStopsWhenToldTo) {
  DeriveOptions pages;
  pages.page_size = 1;
  int asked = 0;
  auto stats = derive_ages(source(), arar_domain(), pages, [&](const DeriveStats&) {
    ++asked;
    return false;
  });
  ASSERT_TRUE(stats) << to_string(stats.error());
  EXPECT_EQ(asked, 1);
  EXPECT_EQ(stats->seen, 1);  // the second page was never read
  EXPECT_EQ(cached(air_).size(), 1u);
  EXPECT_TRUE(cached(unknown_).empty());
}

// The store source, but one analysis does not load.
class OneFailsToLoad final : public IDeriveSource {
 public:
  OneFailsToLoad(StoreSource& source, std::string broken) : source_(source), broken_(std::move(broken)) {}
  Result<BrowsePage> browse(const BrowseQuery& query) override { return source_.browse(query); }
  Result<std::map<std::string, StoreSource::Fingerprints>> fingerprints(
      const std::vector<std::string>& analyses) override {
    return source_.fingerprints(analyses);
  }
  Result<AnalysisPtr> load(const std::string& uuid) override {
    if (uuid == broken_) return fail(ErrorKind::Protocol, "a payload that does not parse", "store");
    return source_.load(uuid);
  }
  Result<std::size_t> write(const std::vector<DerivedValues>& ages) override { return source_.write_derived_now(ages); }
  void drop_loaded(const std::vector<std::string>& analyses) override { source_.drop_loaded(analyses); }

 private:
  StoreSource& source_;
  std::string broken_;
};

// The store source, but it refuses every write.
class RefusesWrites final : public IDeriveSource {
 public:
  explicit RefusesWrites(StoreSource& source) : source_(source) {}
  Result<BrowsePage> browse(const BrowseQuery& query) override { return source_.browse(query); }
  Result<std::map<std::string, StoreSource::Fingerprints>> fingerprints(
      const std::vector<std::string>& analyses) override {
    return source_.fingerprints(analyses);
  }
  Result<AnalysisPtr> load(const std::string& uuid) override { return source_.load(uuid); }
  Result<std::size_t> write(const std::vector<DerivedValues>&) override {
    ++writes;
    return fail(ErrorKind::Io, "disk full", "store");
  }
  void drop_loaded(const std::vector<std::string>& analyses) override { source_.drop_loaded(analyses); }
  int writes = 0;

 private:
  StoreSource& source_;
};

TEST_F(StoreSourceTest, ARefusedWriteStopsTheWalk) {
  RefusesWrites src(source());
  DeriveOptions pages;
  pages.page_size = 1;
  auto stats = derive_ages(src, arar_domain(), pages, always);
  ASSERT_FALSE(stats);
  EXPECT_EQ(stats.error().what, "disk full");
  EXPECT_EQ(src.writes, 1);
}

TEST_F(StoreSourceTest, AFailingLoadIsCountedNamedAndPassedOver) {
  OneFailsToLoad src(source(), unknown_.str());
  auto stats = derive_ages(src, arar_domain(), {}, always);
  ASSERT_TRUE(stats) << to_string(stats.error());
  EXPECT_EQ(stats->failed, 1);
  ASSERT_EQ(stats->failures.size(), 1u);
  EXPECT_NE(stats->failures[0].find("77000"), std::string::npos) << stats->failures[0];
  EXPECT_NE(stats->failures[0].find("does not parse"), std::string::npos) << stats->failures[0];
  EXPECT_EQ(stats->written, 1);  // the air, after it
  EXPECT_TRUE(cached(unknown_).empty());
}

// The production and chronology an irradiated unknown reduces with.
ps::ProductionValue production_ref() { return ps::ProductionValue{"Triga", std::nullopt, {{"K4039", 0.0008, 5e-5}}}; }
ps::ChronologyValue chronology_ref() {
  return ps::ChronologyValue{
      {{0, 1.0, *ps::UtcTime::parse("2026-01-01T00:00:00Z"), *ps::UtcTime::parse("2026-01-01T10:00:00Z")}}};
}

ReducedPtr reduce_parts(const StoreAnalysisParts& parts) {
  auto a = analysis_from_store(parts, arar::store_extension_loader());
  EXPECT_TRUE(a) << (a ? "" : to_string(a.error()));
  if (!a) return nullptr;
  return arar::reduce_with_arar(std::make_shared<const Analysis>(std::move(*a)));
}

// A NULL value or error in a stored row is unknown, not 0: it reaches the
// model as NaN and the analysis does not reduce.

TEST(StoreSourceMapping, MissingStoredNumbersAreUnknownNotZero) {
  {
    const auto reduced = reduce_parts(argon_parts());
    ASSERT_TRUE(reduced);
    EXPECT_TRUE(ar_of(reduced)) << reduced->reduction_error;
    for (const auto& iso : reduced->analysis->isotopes) {
      EXPECT_TRUE(iso.intercept.known());
      EXPECT_TRUE(iso.baseline.known());
      EXPECT_TRUE(iso.blank.known());
      EXPECT_TRUE(iso.ic_factor.known());
    }
  }

  struct Case {
    const char* what;  // as reduction_error names it
    std::optional<double>& (*field)(StoreAnalysisParts&);
    const Value& (*mapped)(const IsotopeData&);
    double other;  // the number left in place
    Stage stage;
    bool is_error;
  };
  const auto cases = std::to_array<Case>({
      {"Ar40 intercept value", [](StoreAnalysisParts& p) -> auto& { return first_row<ps::Intercepts>(p, ps::Kind::Intercepts).value; },
       [](const IsotopeData& i) -> const Value& { return i.intercept; }, 0.1, Stage::Intercept, false},
      {"Ar40 intercept error", [](StoreAnalysisParts& p) -> auto& { return first_row<ps::Intercepts>(p, ps::Kind::Intercepts).error; },
       [](const IsotopeData& i) -> const Value& { return i.intercept; }, 100.0, Stage::Intercept, true},
      {"Ar40 baseline value", [](StoreAnalysisParts& p) -> auto& { return first_row<ps::Baselines>(p, ps::Kind::Baselines).value; },
       [](const IsotopeData& i) -> const Value& { return i.baseline; }, 0.001, Stage::Baseline, false},
      {"Ar40 baseline error", [](StoreAnalysisParts& p) -> auto& { return first_row<ps::Baselines>(p, ps::Kind::Baselines).error; },
       [](const IsotopeData& i) -> const Value& { return i.baseline; }, 0.01, Stage::Baseline, true},
      {"Ar40 blank value", [](StoreAnalysisParts& p) -> auto& { return first_row<ps::Blanks>(p, ps::Kind::Blanks).value; },
       [](const IsotopeData& i) -> const Value& { return i.blank; }, 0.05, Stage::Blank, false},
      {"Ar40 blank error", [](StoreAnalysisParts& p) -> auto& { return first_row<ps::Blanks>(p, ps::Kind::Blanks).error; },
       [](const IsotopeData& i) -> const Value& { return i.blank; }, 0.5, Stage::Blank, true},
      {"Ar40 IC factor value", [](StoreAnalysisParts& p) -> auto& { return first_row<ps::IcFactors>(p, ps::Kind::IcFactors).value; },
       [](const IsotopeData& i) -> const Value& { return i.ic_factor; }, 0.001, Stage::IcFactor, false},
      {"Ar40 IC factor error", [](StoreAnalysisParts& p) -> auto& { return first_row<ps::IcFactors>(p, ps::Kind::IcFactors).error; },
       [](const IsotopeData& i) -> const Value& { return i.ic_factor; }, 1.02, Stage::IcFactor, true},
  });
  for (const auto& c : cases) {
    SCOPED_TRACE(c.what);
    auto parts = argon_parts();
    c.field(parts).reset();
    const auto reduced = reduce_parts(parts);
    ASSERT_TRUE(reduced);
    const IsotopeData* ar40 = reduced->analysis->find_isotope("Ar40");
    ASSERT_TRUE(ar40);
    const Value& v = c.mapped(*ar40);
    EXPECT_FALSE(v.known());
    EXPECT_TRUE(std::isnan(c.is_error ? v.error : v.value));
    EXPECT_EQ(c.is_error ? v.value : v.error, c.other);  // the stored half survives

    EXPECT_FALSE(ar_of(reduced));
    EXPECT_NE(reduced->reduction_error.find("not reducible"), std::string::npos) << reduced->reduction_error;
    EXPECT_NE(reduced->reduction_error.find(c.what), std::string::npos) << reduced->reduction_error;
    // The stage is not a number either, whichever half is missing.
    const auto stage = reduced->stage("Ar40", c.stage);
    ASSERT_TRUE(stage);
    EXPECT_FALSE(std::isfinite(stage->nominal()) && std::isfinite(stage->std_dev()));
    const auto corrected = reduced->stage("Ar40", Stage::IcCorrected);
    ASSERT_TRUE(corrected);
    EXPECT_FALSE(std::isfinite(corrected->nominal()) && std::isfinite(corrected->std_dev()));
  }
}

TEST(StoreSourceMapping, MissingStoredNumbersOverridesAndScope) {
  // A manual override supplies the missing number.
  {
    auto parts = argon_parts();
    auto& row = first_row<ps::Intercepts>(parts, ps::Kind::Intercepts);
    row.value.reset();
    row.error.reset();
    row.manual = ps::ManualOverride{true, 90.0, true, 0.2};
    const auto reduced = reduce_parts(parts);
    ASSERT_TRUE(reduced);
    EXPECT_EQ(reduced->analysis->find_isotope("Ar40")->intercept, (Value{90.0, 0.2}));
    EXPECT_TRUE(ar_of(reduced)) << reduced->reduction_error;
  }
  // A flagged override that holds no number does not.
  {
    auto parts = argon_parts();
    auto& row = first_row<ps::Intercepts>(parts, ps::Kind::Intercepts);
    row.value.reset();
    row.manual.use_value = true;
    const auto reduced = reduce_parts(parts);
    ASSERT_TRUE(reduced);
    EXPECT_FALSE(reduced->analysis->find_isotope("Ar40")->intercept.known());
    EXPECT_FALSE(ar_of(reduced));
  }
  // No row at all is still "none": blank 0, IC factor 1.
  {
    auto parts = argon_parts();
    parts.heads.erase(ps::Kind::Blanks);
    parts.heads.erase(ps::Kind::IcFactors);
    const auto reduced = reduce_parts(parts);
    ASSERT_TRUE(reduced);
    EXPECT_EQ(reduced->analysis->find_isotope("Ar40")->blank, (Value{0.0, 0.0}));
    EXPECT_EQ(reduced->analysis->find_isotope("Ar40")->ic_factor, (Value{1.0, 0.0}));
    EXPECT_TRUE(ar_of(reduced)) << reduced->reduction_error;
  }
  // Nor is an IC factor stub row, with neither a value nor an error.
  {
    auto parts = argon_parts();
    auto& ic = first_row<ps::IcFactors>(parts, ps::Kind::IcFactors);
    ic.value.reset();
    ic.error.reset();
    ic.reviewed = true;
    const auto reduced = reduce_parts(parts);
    ASSERT_TRUE(reduced);
    EXPECT_EQ(reduced->analysis->find_isotope("Ar40")->ic_factor, (Value{1.0, 0.0}));
    EXPECT_FALSE(reduced->analysis->find_isotope("Ar40")->ic_reviewed);
    EXPECT_TRUE(ar_of(reduced)) << reduced->reduction_error;
  }
  // Unless the importer noted that its value or error was not finite: then
  // the factor is unknown.
  for (const char* extra : {R"({"nonfinite": {"/value": "NaN", "/error": "Infinity"}})",
                            R"({"note": "x", "nonfinite": {"/value": "NaN"}})",
                            R"({"nonfinite": {"/error": "NaN"}})"}) {
    SCOPED_TRACE(extra);
    auto parts = argon_parts();
    auto& ic = first_row<ps::IcFactors>(parts, ps::Kind::IcFactors);
    ic.value.reset();
    ic.error.reset();
    ic.extra_json = extra;
    const auto reduced = reduce_parts(parts);
    ASSERT_TRUE(reduced);
    EXPECT_FALSE(reduced->analysis->find_isotope("Ar40")->ic_factor.known());
    EXPECT_FALSE(ar_of(reduced));
    EXPECT_NE(reduced->reduction_error.find("Ar40 IC factor value"), std::string::npos) << reduced->reduction_error;
  }
  // A note about some other field leaves it a stub.
  {
    auto parts = argon_parts();
    auto& ic = first_row<ps::IcFactors>(parts, ps::Kind::IcFactors);
    ic.value.reset();
    ic.error.reset();
    ic.extra_json = R"({"nonfinite": {"/standard_ratio": "NaN"}, "value": "/value"})";
    const auto reduced = reduce_parts(parts);
    ASSERT_TRUE(reduced);
    EXPECT_EQ(reduced->analysis->find_isotope("Ar40")->ic_factor, (Value{1.0, 0.0}));
    EXPECT_TRUE(ar_of(reduced)) << reduced->reduction_error;
  }
  // An isotope reduce() never sees does not stop it.
  {
    auto parts = argon_parts();
    ps::InterceptRow extra;
    extra.isotope = "Ar41";
    extra.detector = "CDD";
    std::get<ps::Intercepts>(parts.heads.at(ps::Kind::Intercepts)).push_back(extra);
    const auto reduced = reduce_parts(parts);
    ASSERT_TRUE(reduced);
    EXPECT_FALSE(reduced->analysis->find_isotope("Ar41")->intercept.known());
    EXPECT_TRUE(ar_of(reduced)) << reduced->reduction_error;
    EXPECT_TRUE(std::isnan(reduced->stage("Ar41", Stage::Intercept)->nominal()));
  }
}

// A flux with a J and a NULL error: the error is unknown, not 0.
TEST(StoreSourceMapping, MissingFluxErrorsAreUnknownNotZero) {
  auto flux = [] {
    ps::FluxValue f;
    f.j = 0.001;
    f.j_err = 1e-6;
    f.position_jerr = 2e-7;
    f.lambda_k_total = 5.5e-10;
    f.lambda_k_total_err = 1e-12;
    return f;
  };
  {
    auto parts = argon_parts();
    parts.refs = {flux(), production_ref(), chronology_ref()};
    const auto reduced = reduce_parts(parts);
    ASSERT_TRUE(reduced);
    ASSERT_TRUE(ar_of(reduced)) << reduced->reduction_error;
    EXPECT_TRUE(ar_of(reduced)->arar.ages);
    EXPECT_TRUE(ar_of(reduced) && ar_of(reduced)->j);
  }

  struct Case {
    const char* what;
    std::optional<double> ps::FluxValue::* field;
    double (*mapped)(const reduction::Flux&);
  };
  const auto cases = std::to_array<Case>({
      {"J error", &ps::FluxValue::j_err, [](const reduction::Flux& f) { return f.j.error; }},
      {"lambda_k_total error", &ps::FluxValue::lambda_k_total_err,
       [](const reduction::Flux& f) { return f.lambda_k_total->error; }},
  });
  for (const auto& c : cases) {
    SCOPED_TRACE(c.what);
    auto f = flux();
    (f.*c.field).reset();
    auto parts = argon_parts();
    parts.refs = {f};
    const auto reduced = reduce_parts(parts);
    ASSERT_TRUE(reduced);
    ASSERT_TRUE(refs_of(*reduced->analysis).flux);
    EXPECT_TRUE(std::isnan(c.mapped(*refs_of(*reduced->analysis).flux)));
    EXPECT_EQ(refs_of(*reduced->analysis).flux->j.value, 0.001);
    EXPECT_FALSE(ar_of(reduced));
    EXPECT_FALSE(ar_of(reduced) && ar_of(reduced)->j);
    EXPECT_NE(reduced->reduction_error.find(std::string("no stored ") + c.what), std::string::npos)
        << reduced->reduction_error;
  }

  // The J quantity of an unknown J error is not a number.
  {
    auto f = flux();
    f.j_err.reset();
    auto parts = argon_parts();
    parts.refs = {f};
    const auto reduced = reduce_parts(parts);
    ASSERT_TRUE(reduced);
    auto q = Quantity::parse("j");
    ASSERT_TRUE(q) << to_string(q.error());
    const auto j = q->eval(*reduced);
    EXPECT_TRUE(!j || !j->known());
  }

  // No position error is none (0), and the ages stand.
  {
    auto f = flux();
    f.position_jerr.reset();
    auto parts = argon_parts();
    parts.refs = {f, production_ref(), chronology_ref()};
    const auto reduced = reduce_parts(parts);
    ASSERT_TRUE(reduced);
    EXPECT_EQ(refs_of(*reduced->analysis).flux->position_jerr, 0.0);
    ASSERT_TRUE(ar_of(reduced)) << reduced->reduction_error;
    EXPECT_TRUE(ar_of(reduced)->arar.ages);
  }

  // No J is no flux: reduced, without ages. No lambda_k_total is no override.
  {
    auto f = flux();
    f.j.reset();
    auto parts = argon_parts();
    parts.refs = {f};
    const auto reduced = reduce_parts(parts);
    ASSERT_TRUE(reduced);
    EXPECT_FALSE(refs_of(*reduced->analysis).flux);
    ASSERT_TRUE(ar_of(reduced)) << reduced->reduction_error;
    EXPECT_FALSE(ar_of(reduced)->arar.ages);
  }
  {
    auto f = flux();
    f.lambda_k_total.reset();
    f.lambda_k_total_err.reset();
    auto parts = argon_parts();
    parts.refs = {f};
    const auto reduced = reduce_parts(parts);
    ASSERT_TRUE(reduced);
    EXPECT_FALSE(refs_of(*reduced->analysis).flux->lambda_k_total);
    EXPECT_TRUE(ar_of(reduced)) << reduced->reduction_error;
  }
}

// An irradiated unknown whose production or chronology is missing (removed in
// the reference-data history) is not given an age from zero interference
// corrections or no decay correction: its ratios stand, it has no J and no
// age, and reduction_error names what is missing.
TEST(StoreSourceMapping, UnknownWithoutProductionOrChronologyHasNoAge) {
  ps::FluxValue flux;
  flux.j = 0.001;
  flux.j_err = 1e-6;
  {
    auto parts = argon_parts();
    parts.refs = {flux, production_ref(), chronology_ref()};
    const auto reduced = reduce_parts(parts);
    ASSERT_TRUE(reduced);
    ASSERT_TRUE(ar_of(reduced)) << reduced->reduction_error;
    EXPECT_TRUE(ar_of(reduced)->arar.ages);
    EXPECT_TRUE(ar_of(reduced) && ar_of(reduced)->j);
    EXPECT_EQ(reduced->reduction_error, "");
    EXPECT_NE(ar_of(reduced)->arar.decay.df39, 1.0);
  }

  struct Case {
    const char* what;
    std::vector<ps::RefPayload> refs;
    const char* error;
  };
  const auto cases = std::to_array<Case>({
      {"no production", {flux, chronology_ref()}, "no age: no production ratios"},
      {"no chronology", {flux, production_ref()}, "no age: no chronology"},
      {"neither", {flux}, "no age: no production ratios, no chronology"},
  });
  for (const auto& c : cases) {
    SCOPED_TRACE(c.what);
    auto parts = argon_parts();
    parts.refs = c.refs;
    const auto reduced = reduce_parts(parts);
    ASSERT_TRUE(reduced);
    ASSERT_TRUE(refs_of(*reduced->analysis).flux);
    ASSERT_TRUE(ar_of(reduced)) << reduced->reduction_error;
    EXPECT_FALSE(ar_of(reduced)->arar.ages);
    EXPECT_FALSE(ar_of(reduced) && ar_of(reduced)->j);
    EXPECT_EQ(reduced->reduction_error, c.error);
    EXPECT_TRUE(ar_of(reduced)->arar.f.f);
    EXPECT_FALSE(Quantity::parse("age")->eval(*reduced));

    // Recall says why there is no age.
    const auto recall = make_recall_model(*reduced);
    EXPECT_EQ(recall.reduction_note, c.error);
  }

  // A stored number the source does not have still wins: nothing reduces.
  {
    auto parts = argon_parts();
    first_row<ps::Intercepts>(parts, ps::Kind::Intercepts).error.reset();
    parts.refs = {flux};
    const auto reduced = reduce_parts(parts);
    ASSERT_TRUE(reduced);
    EXPECT_FALSE(ar_of(reduced));
    EXPECT_NE(reduced->reduction_error.find("not reducible"), std::string::npos) << reduced->reduction_error;
  }
}

// Airs, blanks and cocktails are not irradiated: no flux, production or
// chronology is no reduction error.
TEST(StoreSourceMapping, UnirradiatedTypesNeedNoProductionOrChronology) {
  for (const char* type : {"air", "cocktail", "blank_unknown", "blank_air", "blank_cocktail"}) {
    SCOPED_TRACE(type);
    auto parts = argon_parts();
    parts.detail.row.summary.analysis_type = type;
    const auto reduced = reduce_parts(parts);
    ASSERT_TRUE(reduced);
    EXPECT_FALSE(refs_of(*reduced->analysis).flux);
    EXPECT_FALSE(refs_of(*reduced->analysis).production);
    EXPECT_TRUE(refs_of(*reduced->analysis).chronology.empty());
    ASSERT_TRUE(ar_of(reduced)) << reduced->reduction_error;
    EXPECT_EQ(reduced->reduction_error, "");
    EXPECT_EQ(ar_of(reduced)->arar.decay.df39, 1.0);
  }
}

// The same through a real store: a NULL column comes back unknown.
TEST_F(StoreSourceTest, NullInterceptLoadsAsUnknownAndDoesNotReduce) {
  const auto head = *store_->head(unknown_, ps::Kind::Intercepts);
  auto rows = std::get<ps::Intercepts>(**store_->load_payload(*head));
  auto ar40 = std::find_if(rows.begin(), rows.end(), [](const ps::InterceptRow& r) { return r.isotope == "Ar40"; });
  ASSERT_NE(ar40, rows.end());
  ar40->value.reset();
  auto uow = *store_->begin(reducer());
  ASSERT_TRUE(uow->add_revision(unknown_, ps::Kind::Intercepts, ps::RevisionPayload{std::move(rows)}, *head));
  auto outcome = uow->commit(ps::ChangesetKind::Reduction, "legacy NaN");
  ASSERT_TRUE(outcome && std::holds_alternative<ps::Committed>(*outcome));

  auto loaded = source().load(unknown_.str());
  ASSERT_TRUE(loaded) << to_string(loaded.error());
  const IsotopeData* iso = (*loaded)->find_isotope("Ar40");
  ASSERT_TRUE(iso);
  EXPECT_TRUE(std::isnan(iso->intercept.value));
  EXPECT_EQ(iso->intercept.error, 0.5);
  const auto reduced = arar::reduce_with_arar(*loaded);
  EXPECT_FALSE(ar_of(reduced));
  EXPECT_NE(reduced->reduction_error.find("Ar40 intercept value"), std::string::npos) << reduced->reduction_error;
}

}  // namespace
}  // namespace pychron::processing

namespace pychron::arar::test {
namespace {

namespace pp = pychron::processing;
namespace ps = pychron::persistence;

// The seeded store read with the Ar-Ar loader, as the applications open it
// under arar.
class StoreLoader : public pp::test::StoreSourceFixture {
 protected:
  [[nodiscard]] pp::StoreSourceOptions source_options() const override {
    return store_source_options(StoreSourceFixture::source_options());
  }
};

// What the store source read of the references before the domain seam, now
// the extension the Ar-Ar loader makes.
TEST_F(StoreLoader, LoadsFluxProductionChronology) {
  auto loaded = source().load(unknown_.str());
  ASSERT_TRUE(loaded) << to_string(loaded.error());
  const ArArAnalysisData* data = analysis_data_of(**loaded);
  ASSERT_NE(data, nullptr);
  EXPECT_EQ(data->domain(), "arar");
  ASSERT_TRUE(data->flux);
  EXPECT_EQ(data->flux->j.value, 0.001);
  EXPECT_EQ(data->flux->j.error, 1e-6);
  EXPECT_EQ(data->flux->position_jerr, 2e-7);
  ASSERT_TRUE(data->production);
  EXPECT_EQ(data->production->k4039.value, 0.0008);
  EXPECT_EQ(data->production->ca3937.value, 0.0007);
  ASSERT_EQ(data->chronology.size(), 1u);
  EXPECT_EQ(data->chronology[0].end_utc_s - data->chronology[0].start_utc_s, 36000);
  EXPECT_EQ(data->reactor, "Triga");
  // The neutral parts are the store source's as before.
  EXPECT_EQ((*loaded)->monitor.name, "FC-2");
  EXPECT_EQ((*loaded)->gains.at("L2"), 0.98);

  const auto reduced = reduce_with_arar(*loaded);
  ASSERT_TRUE(reduced_of(*reduced)) << reduced->reduction_error;
  ASSERT_TRUE(reduced_of(*reduced)->arar.ages);

  // The air has no irradiation position, hence no flux and no age; it has
  // Ar-Ar data all the same, and reduces.
  auto air = source().load(air_.str());
  ASSERT_TRUE(air) << to_string(air.error());
  const ArArAnalysisData* air_data = analysis_data_of(**air);
  ASSERT_NE(air_data, nullptr);
  EXPECT_FALSE(air_data->flux);
  EXPECT_FALSE(air_data->production);
  const auto air_reduced = reduce_with_arar(*air);
  ASSERT_TRUE(reduced_of(*air_reduced)) << air_reduced->reduction_error;
  EXPECT_FALSE(reduced_of(*air_reduced)->arar.ages);
}

// The input fingerprint the age cache keys an analysis by: the same bytes as
// before the Ar-Ar inputs moved behind the loader (the fixture was recorded
// from the build before the move), the store's own under the domain's
// reduction version, and changed by a new J.
class StoreSource : public StoreLoader {};

TEST_F(StoreSource, FingerprintUnchangedByTheMoveAndChangedByJ) {
  // (a) Fixed heads and references: the hex and the canonical text recorded
  // before the move.
  std::ifstream in(std::filesystem::path(PYCHRON_ARAR_FIXTURE_DIR) / "fingerprint_before_move.txt", std::ios::binary);
  ASSERT_TRUE(in);
  std::string hex;
  std::getline(in, hex);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  ASSERT_EQ(hex.size(), 64u);
  EXPECT_EQ(to_hex(sha256(std::string_view{text})), hex);  // the text format is pinned too
  const auto id = [](const char* uuid) { return *ps::Uuid::parse(uuid); };
  const ps::Uuid subject = id("00000000-0000-7000-8000-000000000001");
  const std::vector<ps::HeadInfo> heads{
      {subject, ps::Kind::Intercepts, id("00000000-0000-7000-8000-00000000000a"), 1},
      {subject, ps::Kind::Blanks, id("00000000-0000-7000-8000-00000000000b"), 1},
      {subject, ps::Kind::Tags, id("00000000-0000-7000-8000-00000000000c"), 1},
  };
  const std::vector<ps::ResolvedRef> refs{
      {id("00000000-0000-7000-8000-000000000101"), ps::RefType::FluxPosition, "NM-300/A/3",
       id("00000000-0000-7000-8000-000000000111"), false},
      {id("00000000-0000-7000-8000-000000000102"), ps::RefType::Chronology, "NM-300",
       id("00000000-0000-7000-8000-000000000112"), false},
  };
  const std::string version(shared_domain().reduction_version());
  EXPECT_EQ(version, "arar-1");
  EXPECT_EQ(to_hex(ps::fingerprint_of(heads, refs, version)), hex);

  // (b) A load under the domain's version carries the store's fingerprint.
  pp::StoreSource& src = source();
  auto loaded = src.load(unknown_.str());
  ASSERT_TRUE(loaded) << to_string(loaded.error());
  const std::string before = (*loaded)->input_fingerprint;
  EXPECT_EQ(before, to_hex(*store_->input_fingerprint(unknown_, "arar-1")));

  // (c) A new flux revision for the analysis's position: a new input, and the
  // loader reads its J.
  ps::FluxValue flux;
  flux.j = 0.002;
  flux.j_err = 2e-6;
  publish(flux_object_, flux);
  ASSERT_TRUE(src.refresh());
  auto again = src.load(unknown_.str());
  ASSERT_TRUE(again) << to_string(again.error());
  EXPECT_NE((*again)->input_fingerprint, before);
  EXPECT_EQ((*again)->input_fingerprint, to_hex(*store_->input_fingerprint(unknown_, "arar-1")));
  const ArArAnalysisData* data = analysis_data_of(**again);
  ASSERT_NE(data, nullptr);
  ASSERT_TRUE(data->flux);
  EXPECT_EQ(data->flux->j.value, 0.002);
}

// A source opened without the Ar-Ar loader (as under noble_gas): its
// analyses carry no Ar-Ar data, and arar_reduce says so for each.
class ArArReduceUnit : public pp::test::StoreSourceFixture {};

TEST_F(ArArReduceUnit, WithoutTheLoaderNothingReduces) {
  const auto domain = make_domain();
  const auto& reg = domain->units();
  pp::Pipeline p;
  auto& select = p.add(reg, "select", "select");
  ASSERT_TRUE(select.options.set("uuids", std::vector<std::string>{unknown_.str(), air_.str()}));
  p.add(reg, "reduce", "reduce", {"select"});
  p.add(reg, "arar_reduce", "arar_reduce", {"reduce"});
  pp::Runner runner(reg, &source(), domain.get());
  auto out = runner.run(p, "arar_reduce");
  ASSERT_TRUE(out) << to_string(out.error());
  const auto& items = std::get<pp::DatasetPtr>(out->at(0))->items();
  ASSERT_EQ(items.size(), 2u);
  for (const auto& item : items) {
    SCOPED_TRACE(item.analysis->analysis->runid);
    EXPECT_FALSE(item.analysis->analysis->extension);
    EXPECT_FALSE(reduced_of(*item.analysis));
    EXPECT_EQ(item.analysis->reduction_error, "no Ar-Ar reference data for this analysis");
  }
  // Nothing to cache either.
  const auto done = source().flush_derived();
  EXPECT_EQ(done.written, 0u);
}

}  // namespace
}  // namespace pychron::arar::test
