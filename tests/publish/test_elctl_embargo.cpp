// elctl embargo (data publishing design, sections 4.2 and 6.2), driven as
// the binary runs it.

#include <gtest/gtest.h>

#include <sstream>

#include "cli.hpp"
#include "release_fixture.hpp"

namespace ps = pychron::persistence;
using namespace pychron;
using pychron::publish::testing::ReleaseFixture;

namespace {

struct Outcome {
  int code = 0;
  std::string out, err;
};

class ElctlEmbargoTest : public ReleaseFixture {
 protected:
  void SetUp() override {
    if (GetParam() != "sqlite") GTEST_SKIP() << "the command is driven on SQLite; the store is tested on both";
    ReleaseFixture::SetUp();
  }

  Outcome run(std::vector<std::string> args) {
    std::istringstream in;
    std::ostringstream out, err;
    args.insert(args.begin(), "embargo");
    Outcome r;
    r.code = elctl::run(args, elctl::Io{in, out, err});
    r.out = out.str();
    r.err = err.str();
    return r;
  }
};

}  // namespace

TEST_P(ElctlEmbargoTest, SetProjectThenListShowsIt) {
  const Outcome s = run({"set", "--db", db_->url(), "--project", "Alpha", "--reason", "paper in review"});
  EXPECT_EQ(s.code, elctl::kOk) << s.err;
  EXPECT_NE(s.out.find("project Alpha"), std::string::npos) << s.out;
  const Outcome l = run({"list", "--db", db_->url(), "--live"});
  EXPECT_EQ(l.code, elctl::kOk) << l.err;
  EXPECT_NE(l.out.find("project Alpha"), std::string::npos) << l.out;
  EXPECT_NE(l.out.find("indefinite"), std::string::npos) << l.out;
  EXPECT_NE(l.out.find("paper in review"), std::string::npos) << l.out;
  const std::vector<ps::Uuid> samples{sample1_, sample2_, sample3_};
  EXPECT_EQ(store_->embargoed_samples(samples, ps::UtcTime::now())->size(), 2u);
}

TEST_P(ElctlEmbargoTest, LiftByScopeLiftsEveryLiveOne) {
  ASSERT_EQ(run({"set", "--db", db_->url(), "--sample", "FC-1", "--reason", "one"}).code, elctl::kOk);
  ASSERT_EQ(run({"set", "--db", db_->url(), "--sample", "FC-1@Alpha", "--until", "2027-12-31"}).code, elctl::kOk);
  ASSERT_EQ(run({"set", "--db", db_->url(), "--analysis", "70003-01"}).code, elctl::kOk);
  const Outcome l = run({"lift", "--db", db_->url(), "--sample", "FC-1"});
  EXPECT_EQ(l.code, elctl::kOk) << l.err;
  EXPECT_NE(l.out.find("lifted 2"), std::string::npos) << l.out;
  const Outcome live = run({"list", "--db", db_->url(), "--live"});
  EXPECT_EQ(live.out.find("sample FC-1"), std::string::npos) << live.out;
  EXPECT_NE(live.out.find("analysis 70003-01"), std::string::npos) << live.out;
  const Outcome all = run({"list", "--db", db_->url()});
  EXPECT_NE(all.out.find("lifted "), std::string::npos) << all.out;
  // By id too.
  const std::string id = all.out.substr(0, 36);
  const Outcome by_id = run({"lift", "--db", db_->url(), "--id", id});
  EXPECT_TRUE(by_id.code == elctl::kOk || by_id.code == elctl::kUsage) << by_id.err;  // lifted, or already lifted
}

TEST_P(ElctlEmbargoTest, UntilIsInclusiveDate) {
  ASSERT_EQ(run({"set", "--db", db_->url(), "--project", "Beta", "--until", "2026-12-31"}).code, elctl::kOk);
  const Outcome l = run({"list", "--db", db_->url()});
  EXPECT_NE(l.out.find("2026-12-31"), std::string::npos) << l.out;
  auto rows = store_->embargoes(false, ps::UtcTime::now());
  ASSERT_TRUE(rows);
  ASSERT_EQ(rows->size(), 1u);
  ASSERT_TRUE((*rows)[0].until);
  EXPECT_EQ((*rows)[0].until->iso(), "2027-01-01T00:00:00.000000Z");
  EXPECT_EQ(run({"set", "--db", db_->url(), "--project", "Beta", "--until", "soon"}).code, elctl::kUsage);
}

TEST_P(ElctlEmbargoTest, AmbiguousSampleNameIsAnError) {
  ASSERT_TRUE(store_->add_sample(lab_.reduction_client, {"FC-1", project2_, material_}));
  const Outcome r = run({"set", "--db", db_->url(), "--sample", "FC-1"});
  EXPECT_EQ(r.code, elctl::kUsage);
  EXPECT_NE(r.err.find("Alpha"), std::string::npos) << r.err;
  EXPECT_NE(r.err.find("Beta"), std::string::npos) << r.err;
  EXPECT_EQ(run({"set", "--db", db_->url()}).code, elctl::kUsage) << "no scope";
  EXPECT_EQ(run({"set", "--project", "Alpha"}).code, elctl::kUsage) << "no db";
  EXPECT_EQ(run({"help"}).code, elctl::kOk);
  EXPECT_EQ(run({}).code, elctl::kUsage);
}

INSTANTIATE_TEST_SUITE_P(Engines, ElctlEmbargoTest, ::testing::ValuesIn(ps::testing::engines()),
                         [](const ::testing::TestParamInfo<std::string>& i) { return i.param; });
