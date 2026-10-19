// elctl publish (data publishing design, section 6.2), driven as the binary
// runs it, against a store of releasable analyses.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include "cli.hpp"
#include "pychron/publish/manifest.hpp"
#include "release_fixture.hpp"

namespace fs = std::filesystem;
namespace ps = pychron::persistence;
using namespace pychron;
using pychron::publish::testing::ReleaseFixture;

namespace {

struct Outcome {
  int code = 0;
  std::string out, err;
};

class ElctlPublishTest : public ReleaseFixture {
 protected:
  void SetUp() override {
    if (GetParam() != "sqlite") GTEST_SKIP() << "the command is driven on SQLite; the store is tested on both";
    ReleaseFixture::SetUp();
    out_ = fs::temp_directory_path() / ("pychron_elctl_publish_" + std::to_string(std::random_device{}()));
    fs::create_directories(out_);
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(out_, ec);
    ReleaseFixture::TearDown();
  }

  Outcome run(std::vector<std::string> args) {
    std::istringstream in;
    std::ostringstream out, err;
    args.insert(args.begin(), "publish");
    Outcome r;
    r.code = elctl::run(args, elctl::Io{in, out, err});
    r.out = out.str();
    r.err = err.str();
    return r;
  }

  std::vector<std::string> create_args(const std::string& name = "r") {
    return {"create", "--db", db_->url(), "--project", "Alpha", "--project", "Beta", "--name", name,
            "--title", "Release " + name, "--author", "J. Ross|0000-0002-1825-0097|NMT", "--description", "Three unknowns"};
  }

  // The uuid `create` printed.
  static std::string uuid_of(const Outcome& r) {
    const auto at = r.out.find("release ");
    if (at == std::string::npos) return {};
    return r.out.substr(at + 8, 36);
  }

  fs::path out_;
};

std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

TEST_P(ElctlPublishTest, CreatePrintsUuidAndCounts) {
  const Outcome r = run(create_args());
  EXPECT_EQ(r.code, elctl::kOk) << r.err;
  EXPECT_NE(r.out.find("release "), std::string::npos) << r.out;
  EXPECT_EQ(uuid_of(r).size(), 36u) << r.out;
  EXPECT_NE(r.out.find("members 3"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("samples 3"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("version 1"), std::string::npos) << r.out;
  auto row = store_->find_release("r", std::nullopt);
  ASSERT_TRUE(row && *row);
  EXPECT_EQ((*row)->licence, "CC-BY-4.0") << "the default";
  EXPECT_EQ((*row)->title, "Release r");
  EXPECT_NE((*row)->options_json.find("\"sigma\":2"), std::string::npos) << (*row)->options_json;
}

TEST_P(ElctlPublishTest, CreateWithUnknownLicenceStillCreatesAndWarns) {
  auto args = create_args("odd");
  args.emplace_back("--licence");
  args.emplace_back("MIT-0");
  const Outcome r = run(args);
  EXPECT_EQ(r.code, elctl::kOk) << r.err;
  EXPECT_NE(r.err.find("MIT-0"), std::string::npos) << r.err;
  EXPECT_NE(r.err.find("warning"), std::string::npos) << r.err;
}

TEST_P(ElctlPublishTest, BuildWritesDirectoryAndVerifyPasses) {
  ASSERT_EQ(run(create_args()).code, elctl::kOk);
  const Outcome b = run({"build", "--db", db_->url(), "--release", "r", "--out", out_.string()});
  EXPECT_EQ(b.code, elctl::kOk) << b.err;
  EXPECT_TRUE(fs::exists(out_ / "r-v1" / "manifest.json")) << b.out << b.err;
  EXPECT_NE(b.out.find((out_ / "r-v1").string()), std::string::npos) << b.out;
  const Outcome v = run({"verify", (out_ / "r-v1").string()});
  EXPECT_EQ(v.code, elctl::kOk) << v.err;
  EXPECT_NE(v.out.find("ok"), std::string::npos) << v.out;
  const Outcome v2 = run({"verify", (out_ / "r-v1").string(), "--db", db_->url()});
  EXPECT_EQ(v2.code, elctl::kOk) << v2.err << v2.out;
}

TEST_P(ElctlPublishTest, VerifyFailsOnAChangedCell) {
  ASSERT_EQ(run(create_args()).code, elctl::kOk);
  ASSERT_EQ(run({"build", "--db", db_->url(), "--release", "r", "--out", out_.string()}).code, elctl::kOk);
  const fs::path csv = out_ / "r-v1" / "tables" / "release.csv";
  std::string text = slurp(csv);
  text[text.size() / 2] = text[text.size() / 2] == 'x' ? 'y' : 'x';
  std::ofstream(csv, std::ios::binary) << text;
  const Outcome v = run({"verify", (out_ / "r-v1").string()});
  EXPECT_EQ(v.code, elctl::kFailed);
  EXPECT_NE(v.err.find("tables/release.csv"), std::string::npos) << v.err;
}

TEST_P(ElctlPublishTest, BuildWithFiguresAndVerifyAgainstTheStore) {
  ASSERT_EQ(run(create_args()).code, elctl::kOk);
  const Outcome b = run({"build", "--db", db_->url(), "--release", "r", "--out", out_.string(), "--with-figures"});
  EXPECT_EQ(b.code, elctl::kOk) << b.err;
  EXPECT_EQ(b.err.find("ignored"), std::string::npos) << b.err;
  EXPECT_TRUE(fs::exists(out_ / "r-v1" / "figures" / "FC-1" / "ideogram.svg")) << b.out << b.err;
  const Outcome v = run({"verify", (out_ / "r-v1").string(), "--db", db_->url()});
  EXPECT_EQ(v.code, elctl::kOk) << v.err << v.out;
  EXPECT_NE(v.out.find("the rebuild agrees"), std::string::npos) << v.out;
}

TEST_P(ElctlPublishTest, VerifyNamesAChangedFigure) {
  ASSERT_EQ(run(create_args()).code, elctl::kOk);
  ASSERT_EQ(run({"build", "--db", db_->url(), "--release", "r", "--out", out_.string(), "--with-figures"}).code,
            elctl::kOk);
  const fs::path svg = out_ / "r-v1" / "figures" / "FC-1" / "ideogram.svg";
  ASSERT_TRUE(fs::exists(svg));
  std::ofstream(svg, std::ios::binary | std::ios::app) << ' ';
  const Outcome v = run({"verify", (out_ / "r-v1").string()});
  EXPECT_EQ(v.code, elctl::kFailed);
  EXPECT_NE(v.err.find("changed figures/FC-1/ideogram.svg"), std::string::npos) << v.err;
}

TEST_P(ElctlPublishTest, PublishRequiresABuildOfTheSamePins) {
  ASSERT_EQ(run(create_args("a")).code, elctl::kOk);
  ASSERT_EQ(run(create_args("b")).code, elctl::kOk);
  ASSERT_EQ(run({"build", "--db", db_->url(), "--release", "b", "--out", out_.string()}).code, elctl::kOk);
  const Outcome wrong = run({"publish", "--db", db_->url(), "--release", "a", "--manifest",
                         (out_ / "b-v1" / "manifest.json").string()});
  EXPECT_EQ(wrong.code, elctl::kUsage) << wrong.err;
  EXPECT_NE(wrong.err.find("pins"), std::string::npos) << wrong.err;
  const Outcome right = run({"publish", "--db", db_->url(), "--release", "b", "--manifest",
                         (out_ / "b-v1" / "manifest.json").string()});
  EXPECT_EQ(right.code, elctl::kOk) << right.err;
  auto row = store_->find_release("b", std::nullopt);
  ASSERT_TRUE(row && *row);
  EXPECT_EQ((*row)->state, ps::ReleaseState::Published);
  ASSERT_TRUE((*row)->manifest_sha256);
  EXPECT_EQ(*(*row)->manifest_sha256, sha256(std::string_view(slurp(out_ / "b-v1" / "manifest.json"))));
}

TEST_P(ElctlPublishTest, PublishKeepsPrivateUnlessPublicFlag) {
  ASSERT_EQ(run(create_args("a")).code, elctl::kOk);
  ASSERT_EQ(run(create_args("b")).code, elctl::kOk);
  ASSERT_EQ(run({"build", "--db", db_->url(), "--release", "a", "--out", out_.string()}).code, elctl::kOk);
  ASSERT_EQ(run({"build", "--db", db_->url(), "--release", "b", "--out", out_.string()}).code, elctl::kOk);
  ASSERT_EQ(run({"publish", "--db", db_->url(), "--release", "a", "--manifest", (out_ / "a-v1" / "manifest.json").string()}).code, elctl::kOk);
  ASSERT_EQ(run({"publish", "--db", db_->url(), "--release", "b", "--manifest", (out_ / "b-v1" / "manifest.json").string(), "--public"}).code, elctl::kOk);
  EXPECT_EQ((*store_->find_release("a", std::nullopt))->visibility, ps::Visibility::Private);
  EXPECT_EQ((*store_->find_release("b", std::nullopt))->visibility, ps::Visibility::Public);
  const Outcome w = run({"withdraw", "--db", db_->url(), "--release", "b", "--reason", "wrong J"});
  EXPECT_EQ(w.code, elctl::kOk) << w.err;
  EXPECT_EQ((*store_->find_release("b", std::nullopt))->state, ps::ReleaseState::Withdrawn);
  EXPECT_EQ(run({"withdraw", "--db", db_->url(), "--release", "a"}).code, elctl::kUsage) << "no reason";
}

TEST_P(ElctlPublishTest, PublishPublicRefusesAPrivateBuild) {
  ASSERT_EQ(run(create_args()).code, elctl::kOk);
  ASSERT_EQ(run({"build", "--db", db_->url(), "--release", "r", "--out", out_.string(), "--private"}).code, elctl::kOk);
  const Outcome pub = run({"publish", "--db", db_->url(), "--release", "r", "--manifest", (out_ / "r-v1" / "manifest.json").string(), "--public"});
  EXPECT_EQ(pub.code, elctl::kUsage) << pub.err;
  EXPECT_NE(pub.err.find("private"), std::string::npos) << pub.err;
  EXPECT_EQ((*store_->find_release("r", std::nullopt))->state, ps::ReleaseState::Draft);
}

TEST_P(ElctlPublishTest, ListAndShow) {
  ASSERT_EQ(run(create_args()).code, elctl::kOk);
  const Outcome l = run({"list", "--db", db_->url()});
  EXPECT_EQ(l.code, elctl::kOk) << l.err;
  EXPECT_NE(l.out.find('r'), std::string::npos);
  EXPECT_NE(l.out.find("v1"), std::string::npos) << l.out;
  EXPECT_NE(l.out.find("draft"), std::string::npos) << l.out;
  EXPECT_NE(l.out.find("private"), std::string::npos) << l.out;
  const Outcome s = run({"show", "--db", db_->url(), "--release", "r@v1"});
  EXPECT_EQ(s.code, elctl::kOk) << s.err;
  EXPECT_NE(s.out.find("members"), std::string::npos) << s.out;
  EXPECT_NE(s.out.find("bookmark"), std::string::npos) << s.out;
  EXPECT_NE(s.out.find("CC-BY-4.0"), std::string::npos) << s.out;
  EXPECT_EQ(run({"show", "--db", db_->url(), "--release", "nope"}).code, elctl::kUsage);
  EXPECT_EQ(run({"list", "--db", db_->url(), "--state", "published"}).out, "no releases\n");
  const Outcome d = run({"delete", "--db", db_->url(), "--release", "r"});
  EXPECT_EQ(d.code, elctl::kOk) << d.err;
  EXPECT_FALSE(*store_->find_release("r", std::nullopt));
}

TEST_P(ElctlPublishTest, WithoutDbIsUsage) {
  const Outcome r = run({"create", "--name", "r", "--title", "t"});
  EXPECT_EQ(r.code, elctl::kUsage);
  EXPECT_EQ(r.err.rfind("elctl publish: --db", 0), 0u) << r.err;
  EXPECT_EQ(run({}).code, elctl::kUsage);
  EXPECT_EQ(run({"nonsense"}).code, elctl::kUsage);
  const Outcome h = run({"help"});
  EXPECT_EQ(h.code, elctl::kOk);
  EXPECT_NE(h.out.find("usage: elctl publish"), std::string::npos);
  EXPECT_EQ(run({"doi", "--db", db_->url()}).code, elctl::kUsage) << "not yet";
}

TEST_P(ElctlPublishTest, NothingMatchedIsOne) {
  const Outcome r = run({"create", "--db", db_->url(), "--project", "Gamma", "--name", "r", "--title", "t"});
  EXPECT_EQ(r.code, elctl::kFailed) << r.err;
}

INSTANTIATE_TEST_SUITE_P(Engines, ElctlPublishTest, ::testing::ValuesIn(ps::testing::engines()),
                         [](const ::testing::TestParamInfo<std::string>& i) { return i.param; });
