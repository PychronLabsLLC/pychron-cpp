#pragma once

// A store with releasable analyses, for the publish tests: two samples of
// one project and one of another, each with one unknown of five argon
// isotopes on a level that has a flux, a production and a chronology (so
// the analyses have ages and references to pin). Engine-parameterised as the
// persistence tests are: SQLite always, PostgreSQL when PYCHRON_TEST_PG_URL
// is set.

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "persistence/store_fixture.hpp"
#include "release_seed.hpp"
#include "pychron/arar/store_loader.hpp"
#include "pychron/persistence/store.hpp"

namespace pychron::publish::testing {

namespace ps = persistence;

// How the tests open a StoreSource on the release store: the Ar-Ar domain's
// version and loader (releases are of Ar-Ar ages), `user` on `host`, two
// connections.
inline processing::StoreSourceOptions source_options(std::string user = "tester", std::string host = "test-host") {
  processing::StoreSourceOptions options;
  options.user = std::move(user);
  options.hostname = std::move(host);
  return arar::store_source_options(std::move(options));
}

class ReleaseFixture : public ::testing::TestWithParam<std::string> {
 protected:
  void SetUp() override {
    db_ = std::make_unique<ps::testing::TestDatabase>(GetParam(), true);
    store_ = ps::testing::open_or_die(db_->url());
    ASSERT_TRUE(store_);
    auto seeded = seed_release_store(*store_);
    ASSERT_TRUE(seeded) << to_string(seeded.error());
    lab_ = seeded->lab;
    project_ = seeded->project;
    project2_ = seeded->project2;
    material_ = seeded->material;
    sample1_ = seeded->sample1;
    sample2_ = seeded->sample2;
    sample3_ = seeded->sample3;
    position1_ = seeded->position1;
    position2_ = seeded->position2;
    production_ = seeded->production;
    analysis1_ = seeded->analysis1;
    analysis2_ = seeded->analysis2;
    analysis3_ = seeded->analysis3;
  }

  ps::Actor reducer() const { return ps::Actor{lab_.reducer, lab_.reduction_client}; }
  std::vector<ps::Uuid> analyses() const { return {analysis1_, analysis2_, analysis3_}; }
  std::vector<std::string> analysis_texts() const {
    return {analysis1_.str(), analysis2_.str(), analysis3_.str()};
  }

  // A tags revision on top of the analysis's head.
  void retag(ps::Uuid analysis, const std::string& tag) {
    auto uow = *store_->begin(reducer());
    ASSERT_TRUE(uow->add_revision(analysis, ps::Kind::Tags, ps::TagValue{tag, std::nullopt, std::nullopt},
                                  *store_->head(analysis, ps::Kind::Tags)));
    auto outcome = uow->commit(ps::ChangesetKind::Reduction, "tag " + tag);
    ASSERT_TRUE(outcome && std::holds_alternative<ps::Committed>(*outcome));
  }

  void publish_ref(ps::Uuid object, ps::RefPayload payload) {
    auto uow = *store_->begin(reducer());
    ASSERT_TRUE(uow->add_revision(object, ps::Kind::RefValue, ps::RevisionPayload{std::move(payload)},
                                  *store_->head(object, ps::Kind::RefValue)));
    auto outcome = uow->commit(ps::ChangesetKind::Reference, "<META> update");
    ASSERT_TRUE(outcome && std::holds_alternative<ps::Committed>(*outcome));
  }

  ps::Uuid ref_object(ps::RefType type, const std::string& key, ps::RefObjectSpec scope) {
    scope.type = type;
    scope.key = key;
    return *store_->add_ref_object(lab_.reduction_client, scope);
  }

  ps::Uuid ingest(const std::string& identifier, int aliquot, const std::string& ts) {
    auto id = ingest_unknown(*store_, lab_, identifier, aliquot, ts);
    EXPECT_TRUE(id) << (id ? "" : to_string(id.error()));
    return id ? *id : ps::Uuid{};
  }

  std::unique_ptr<ps::testing::TestDatabase> db_;  // first: the store holds a connection to it
  std::unique_ptr<ps::IStore> store_;
  ps::testing::Lab lab_;
  ps::Uuid project_, project2_, material_, sample1_, sample2_, sample3_, position1_, position2_, production_,
      analysis1_, analysis2_, analysis3_;
};

}  // namespace pychron::publish::testing
