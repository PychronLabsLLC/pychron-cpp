// GitReader against throwaway repositories made by GitFixture. Every test
// skips when git is not on PATH. Nothing here touches a repository outside
// the fixture's temporary directory.

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "git_fixture.hpp"
#include "pychron/dvc/git_reader.hpp"

namespace pychron::dvc {
namespace {

using testing::GitFixture;

#define SKIP_WITHOUT_GIT() \
  if (!GitFixture::available()) GTEST_SKIP() << "git is not on PATH"

#define ASSERT_OK(result) ASSERT_TRUE((result).has_value()) << to_string((result).error())

GitConfig config_for(const GitFixture& repo, std::size_t cache_bytes = 256u << 20) {
  GitConfig config;
  config.repo = repo.path();
  config.branch = "main";
  config.scratch = repo.temp("scratch");
  config.cache_bytes = cache_bytes;
  return config;
}

// The files left in the scratch directory: none, after any call.
std::size_t scratch_files(const GitFixture& repo) {
  if (!std::filesystem::exists(repo.temp("scratch"))) return 0;
  return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator(repo.temp("scratch")),
                                                std::filesystem::directory_iterator()));
}

const GitChange* find_change(const std::vector<GitChange>& all, std::string_view commit, std::string_view path) {
  for (const auto& change : all)
    if (change.commit == commit && change.path == path) return &change;
  return nullptr;
}

std::vector<std::string> paths_of(const std::vector<GitChange>& all, std::string_view commit) {
  std::vector<std::string> paths;
  for (const auto& change : all)
    if (change.commit == commit) paths.push_back(change.path);
  return paths;
}

TEST(GitReader, LinearHistoryInOrder) {
  SKIP_WITHOUT_GIT();
  GitFixture repo;
  repo.init();
  repo.write("a.json", "1");
  const std::string c1 = repo.commit("one", "2016-03-04T05:06:07+00:00");
  repo.write("a.json", "2");
  const std::string c2 = repo.commit("two", "2016-03-05T05:06:07+00:00");
  repo.write("a.json", "3");
  const std::string c3 = repo.commit("three", "2016-03-06T05:06:07+00:00");

  auto reader = GitReader::open(config_for(repo));
  ASSERT_OK(reader);
  EXPECT_EQ(reader->head(), c3);

  const auto all = reader->rev_list(std::nullopt);
  ASSERT_OK(all);
  EXPECT_EQ(*all, (std::vector<std::string>{c1, c2, c3}));

  const auto rest = reader->rev_list(c1);
  ASSERT_OK(rest);
  EXPECT_EQ(*rest, (std::vector<std::string>{c2, c3}));

  const auto none = reader->rev_list(c3);
  ASSERT_OK(none);
  EXPECT_TRUE(none->empty());
  EXPECT_EQ(scratch_files(repo), 0u);
}

TEST(GitReader, HeadIsFixedAtOpen) {
  SKIP_WITHOUT_GIT();
  GitFixture repo;
  repo.init();
  repo.write("a.json", "1");
  const std::string c1 = repo.commit("one", "2016-03-04T05:06:07+00:00");
  auto reader = GitReader::open(config_for(repo));
  ASSERT_OK(reader);
  repo.write("a.json", "2");
  repo.commit("two", "2016-03-05T05:06:07+00:00");

  EXPECT_EQ(reader->head(), c1);
  const auto all = reader->rev_list(std::nullopt);
  ASSERT_OK(all);
  EXPECT_EQ(*all, (std::vector<std::string>{c1}));
}

TEST(GitReader, RevListAfterACommitOffTheBranchIsAnError) {
  SKIP_WITHOUT_GIT();
  GitFixture repo;
  repo.init();
  repo.write("a.json", "1");
  repo.commit("one", "2016-03-04T05:06:07+00:00");
  repo.branch("side");
  repo.checkout("side");
  repo.write("s.json", "1");
  const std::string side = repo.commit("side", "2016-03-05T05:06:07+00:00");
  repo.checkout("main");

  auto reader = GitReader::open(config_for(repo));
  ASSERT_OK(reader);
  const auto off_branch = reader->rev_list(side);
  ASSERT_FALSE(off_branch.has_value());
  EXPECT_NE(off_branch.error().what.find("not an ancestor"), std::string::npos) << off_branch.error().what;
  EXPECT_NE(off_branch.error().what.find(side), std::string::npos);

  const auto unknown = reader->rev_list(std::string(40, 'd'));
  EXPECT_FALSE(unknown.has_value());
  const auto not_a_sha = reader->rev_list(std::string("--all"));
  EXPECT_FALSE(not_a_sha.has_value());
}

TEST(GitReader, CommitFields) {
  SKIP_WITHOUT_GIT();
  GitFixture repo;
  repo.init();
  repo.write("a.json", "1");
  const std::string c1 = repo.commit("first", "2016-03-04T05:06:07+00:00", "Bo Diddley <bo@example.org>");
  repo.write("a.json", "2");
  const std::string message = "subject line\n\nbody one\n  indented trailing text";
  const std::string c2 = repo.commit(message, "2016-03-04T05:06:07-07:00");
  repo.write("a.json", "3");
  const std::string c3 = repo.commit("east", "2016-01-01T00:30:00+05:30");

  auto reader = GitReader::open(config_for(repo));
  ASSERT_OK(reader);
  // Asked for out of order and twice: answered in the order asked.
  const std::vector<std::string> shas{c2, c1, c3, c2};
  const auto commits = reader->commits(shas);
  ASSERT_OK(commits);
  ASSERT_EQ(commits->size(), 4u);

  const GitCommit& second = (*commits)[0];
  EXPECT_EQ(second.sha, c2);
  EXPECT_EQ(second.parents, (std::vector<std::string>{c1}));
  EXPECT_EQ(second.author.name, "Ann");
  EXPECT_EQ(second.author.email, "ann@example.org");
  EXPECT_EQ(second.message, message);
  EXPECT_EQ(second.author.utc, *persistence::UtcTime::parse("2016-03-04T12:06:07Z"));

  const GitCommit& first = (*commits)[1];
  EXPECT_EQ(first.sha, c1);
  EXPECT_TRUE(first.parents.empty());
  EXPECT_EQ(first.author.name, "Bo Diddley");
  EXPECT_EQ(first.author.email, "bo@example.org");
  EXPECT_EQ(first.message, "first");
  EXPECT_EQ(first.author.utc, *persistence::UtcTime::parse("2016-03-04T05:06:07Z"));

  EXPECT_EQ((*commits)[2].author.utc, *persistence::UtcTime::parse("2015-12-31T19:00:00Z"));
  EXPECT_EQ((*commits)[3].sha, c2);

  EXPECT_TRUE(reader->commits({})->empty());
  const std::vector<std::string> unknown{std::string(40, 'd')};
  EXPECT_FALSE(reader->commits(unknown).has_value());
  EXPECT_EQ(scratch_files(repo), 0u);
}

TEST(GitReader, ChangesAddModifyDelete) {
  SKIP_WITHOUT_GIT();
  GitFixture repo;
  repo.init();
  repo.write("660/52-01E.json", "one");
  repo.write("660/tags/52-01E.tags.json", "tag");
  const std::string c1 = repo.commit("root", "2016-03-04T05:06:07+00:00");
  repo.write("660/52-01E.json", "two");
  repo.write("660/52-01F.json", "new");
  repo.remove("660/tags/52-01E.tags.json");
  const std::string c2 = repo.commit("edit", "2016-03-05T05:06:07+00:00");
  const std::string c3 = repo.commit("nothing", "2016-03-06T05:06:07+00:00");

  auto reader = GitReader::open(config_for(repo));
  ASSERT_OK(reader);
  const std::vector<std::string> shas{c1, c2, c3};
  const auto changes = reader->changes(shas);
  ASSERT_OK(changes);
  ASSERT_EQ(changes->size(), 5u);

  // The root commit against the empty tree.
  EXPECT_EQ(paths_of(*changes, c1), (std::vector<std::string>{"660/52-01E.json", "660/tags/52-01E.tags.json"}));
  for (const auto& change : *changes)
    if (change.commit == c1) EXPECT_EQ(change.status, 'A');

  const GitChange* modified = find_change(*changes, c2, "660/52-01E.json");
  const GitChange* added = find_change(*changes, c2, "660/52-01F.json");
  const GitChange* deleted = find_change(*changes, c2, "660/tags/52-01E.tags.json");
  ASSERT_NE(modified, nullptr);
  ASSERT_NE(added, nullptr);
  ASSERT_NE(deleted, nullptr);
  EXPECT_EQ(modified->status, 'M');
  EXPECT_EQ(added->status, 'A');
  EXPECT_EQ(deleted->status, 'D');
  EXPECT_EQ(deleted->blob_sha, "");
  EXPECT_TRUE(paths_of(*changes, c3).empty());

  // The blob sha names the content at that commit.
  const std::vector<std::string> blobs{modified->blob_sha, added->blob_sha};
  ASSERT_OK(reader->fetch_blobs(blobs));
  EXPECT_EQ(*reader->blob(modified->blob_sha), "two");
  EXPECT_EQ(*reader->blob(added->blob_sha), "new");

  EXPECT_TRUE(reader->changes({})->empty());
  EXPECT_EQ(scratch_files(repo), 0u);
}

TEST(GitReader, RenameIsDeletePlusAdd) {
  SKIP_WITHOUT_GIT();
  GitFixture repo;
  repo.init();
  const std::string text(2000, 'x');
  repo.write("660/52-01E.json", text);
  repo.commit("root", "2016-03-04T05:06:07+00:00");
  repo.remove("660/52-01E.json");
  repo.write("661/52-01E.json", text);
  const std::string c2 = repo.commit("move", "2016-03-05T05:06:07+00:00");

  auto reader = GitReader::open(config_for(repo));
  ASSERT_OK(reader);
  const std::vector<std::string> shas{c2};
  const auto changes = reader->changes(shas);
  ASSERT_OK(changes);
  ASSERT_EQ(changes->size(), 2u);
  const GitChange* gone = find_change(*changes, c2, "660/52-01E.json");
  const GitChange* came = find_change(*changes, c2, "661/52-01E.json");
  ASSERT_NE(gone, nullptr);
  ASSERT_NE(came, nullptr);
  EXPECT_EQ(gone->status, 'D');
  EXPECT_EQ(came->status, 'A');
}

TEST(GitReader, MergeDiffsAgainstFirstParent) {
  SKIP_WITHOUT_GIT();
  GitFixture repo;
  repo.init();
  repo.write("base.json", "base");
  const std::string root = repo.commit("root", "2016-03-04T00:00:00+00:00");
  repo.branch("side");
  repo.write("main.json", "main");
  const std::string on_main = repo.commit("main work", "2016-03-05T00:00:00+00:00");
  repo.checkout("side");
  repo.write("side.json", "side");
  const std::string side1 = repo.commit("side work", "2016-03-06T00:00:00+00:00");
  repo.write("side.json", "side again");
  const std::string side2 = repo.commit("more side work", "2016-03-07T00:00:00+00:00");
  repo.checkout("main");
  const std::string merge = repo.merge("side", "merge side", "2016-03-08T00:00:00+00:00");

  auto reader = GitReader::open(config_for(repo));
  ASSERT_OK(reader);
  const auto order = reader->rev_list(std::nullopt);
  ASSERT_OK(order);
  ASSERT_EQ(order->size(), 5u);
  EXPECT_EQ(order->front(), root);
  EXPECT_EQ(order->back(), merge);
  const auto at = [&](const std::string& sha) { return std::find(order->begin(), order->end(), sha) - order->begin(); };
  EXPECT_LT(at(side1), at(side2));
  EXPECT_LT(at(side2), at(merge));
  EXPECT_LT(at(on_main), at(merge));

  const std::vector<std::string> shas{merge};
  const auto commits = reader->commits(shas);
  ASSERT_OK(commits);
  EXPECT_EQ((*commits)[0].parents, (std::vector<std::string>{on_main, side2}));

  // Against main.json's commit, only side.json is new; main.json (the
  // difference from the second parent) is not listed.
  const auto changes = reader->changes(*order);
  ASSERT_OK(changes);
  EXPECT_EQ(paths_of(*changes, merge), (std::vector<std::string>{"side.json"}));
  EXPECT_EQ(find_change(*changes, merge, "side.json")->status, 'A');
  EXPECT_EQ(paths_of(*changes, side2), (std::vector<std::string>{"side.json"}));
}

TEST(GitReader, MergeEqualToItsFirstParentHasNoChanges) {
  SKIP_WITHOUT_GIT();
  GitFixture repo;
  repo.init();
  repo.write("base.json", "base");
  repo.commit("root", "2016-03-04T00:00:00+00:00");
  repo.branch("side");
  repo.checkout("side");
  repo.write("side.json", "side");
  repo.commit("side work", "2016-03-06T00:00:00+00:00");
  repo.checkout("main");
  repo.git({"merge", "--quiet", "--no-ff", "-s", "ours", "-m", "keep ours", "side"}, "2016-03-08T00:00:00+00:00");
  const std::string merge = repo.head();

  auto reader = GitReader::open(config_for(repo));
  ASSERT_OK(reader);
  const std::vector<std::string> shas{merge};
  const auto changes = reader->changes(shas);
  ASSERT_OK(changes);
  EXPECT_TRUE(changes->empty());
}

TEST(GitReader, OddFileNames) {
  SKIP_WITHOUT_GIT();
  GitFixture repo;
  repo.init();
  std::vector<std::string> names{"664/in tercepts/57 01A.inte.json", "dir/na\xc3\xafve.json", ":colon.json"};
#ifndef _WIN32
  names.push_back("dir/q\"uote.json");  // Windows has no '"' in file names
#endif
  for (const auto& name : names) repo.write(name, "text of " + name);
  const std::string c1 = repo.commit("odd names", "2016-03-04T05:06:07+00:00");

  auto reader = GitReader::open(config_for(repo));
  ASSERT_OK(reader);
  const std::vector<std::string> shas{c1};
  const auto changes = reader->changes(shas);
  ASSERT_OK(changes);
  ASSERT_EQ(changes->size(), names.size());
  for (const auto& name : names) {
    const GitChange* change = find_change(*changes, c1, name);
    ASSERT_NE(change, nullptr) << name;
    const std::vector<std::string> blobs{change->blob_sha};
    ASSERT_OK(reader->fetch_blobs(blobs));
    EXPECT_EQ(*reader->blob(change->blob_sha), "text of " + name);
  }
}

TEST(GitReader, BlobLargerThan64KiB) {
  SKIP_WITHOUT_GIT();
  GitFixture repo;
  repo.init();
  std::string big(300000, '\0');
  for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>((i * 7 + i / 251) % 256);  // NUL, LF, all bytes
  repo.write("big.bin", big);
  repo.write("empty.json", "");
  repo.write("small.json", "\n\n");
  const std::string c1 = repo.commit("big", "2016-03-04T05:06:07+00:00");

  auto reader = GitReader::open(config_for(repo));
  ASSERT_OK(reader);
  const std::vector<std::string> shas{c1};
  const auto changes = reader->changes(shas);
  ASSERT_OK(changes);
  std::vector<std::string> blobs;
  for (const auto& change : *changes) blobs.push_back(change.blob_sha);
  ASSERT_OK(reader->fetch_blobs(blobs));

  const auto whole = reader->blob(find_change(*changes, c1, "big.bin")->blob_sha);
  ASSERT_OK(whole);
  EXPECT_EQ(whole->size(), 300000u);
  EXPECT_TRUE(*whole == big);
  EXPECT_EQ(*reader->blob(find_change(*changes, c1, "empty.json")->blob_sha), "");
  EXPECT_EQ(*reader->blob(find_change(*changes, c1, "small.json")->blob_sha), "\n\n");
  EXPECT_EQ(scratch_files(repo), 0u);
}

TEST(GitReader, BlobCacheEvicts) {
  SKIP_WITHOUT_GIT();
  GitFixture repo;
  repo.init();
  repo.write("a.json", std::string(800, 'a'));
  repo.write("b.json", std::string(800, 'b'));
  repo.write("c.json", std::string(100, 'c'));
  const std::string c1 = repo.commit("blobs", "2016-03-04T05:06:07+00:00");

  auto reader = GitReader::open(config_for(repo, 1000));
  ASSERT_OK(reader);
  const std::vector<std::string> shas{c1};
  const auto changes = reader->changes(shas);
  ASSERT_OK(changes);
  const std::string a = find_change(*changes, c1, "a.json")->blob_sha;
  const std::string b = find_change(*changes, c1, "b.json")->blob_sha;
  const std::string c = find_change(*changes, c1, "c.json")->blob_sha;

  ASSERT_OK(reader->fetch_blobs(std::vector<std::string>{a}));
  EXPECT_EQ(*reader->blob(a), std::string(800, 'a'));

  ASSERT_OK(reader->fetch_blobs(std::vector<std::string>{b}));
  EXPECT_EQ(*reader->blob(b), std::string(800, 'b'));
  const auto evicted = reader->blob(a);
  ASSERT_FALSE(evicted.has_value());
  EXPECT_NE(evicted.error().what.find(a), std::string::npos);

  // 800 + 100 fits: b stays.
  ASSERT_OK(reader->fetch_blobs(std::vector<std::string>{c}));
  EXPECT_EQ(*reader->blob(c), std::string(100, 'c'));
  EXPECT_EQ(*reader->blob(b), std::string(800, 'b'));

  // One call asking for more than the cache holds still serves all of it.
  ASSERT_OK(reader->fetch_blobs(std::vector<std::string>{a, b, a}));
  EXPECT_EQ(*reader->blob(a), std::string(800, 'a'));
  EXPECT_EQ(*reader->blob(b), std::string(800, 'b'));
  EXPECT_FALSE(reader->blob(c).has_value());
}

TEST(GitReader, BlobErrors) {
  SKIP_WITHOUT_GIT();
  GitFixture repo;
  repo.init();
  repo.write("a.json", "a");
  const std::string c1 = repo.commit("one", "2016-03-04T05:06:07+00:00");

  auto reader = GitReader::open(config_for(repo));
  ASSERT_OK(reader);
  const std::string absent(40, 'd');
  EXPECT_FALSE(reader->blob(absent).has_value());  // never fetched

  const auto missing = reader->fetch_blobs(std::vector<std::string>{absent});
  ASSERT_FALSE(missing.has_value());
  EXPECT_NE(missing.error().what.find("missing"), std::string::npos) << missing.error().what;

  const auto commit_not_blob = reader->fetch_blobs(std::vector<std::string>{c1});
  ASSERT_FALSE(commit_not_blob.has_value());
  EXPECT_NE(commit_not_blob.error().what.find("commit"), std::string::npos) << commit_not_blob.error().what;

  EXPECT_FALSE(reader->fetch_blobs(std::vector<std::string>{"--batch-all-objects"}).has_value());
  EXPECT_TRUE(reader->fetch_blobs({}).has_value());
  EXPECT_EQ(scratch_files(repo), 0u);
}

TEST(GitReader, TagsLightweightAndAnnotated) {
  SKIP_WITHOUT_GIT();
  GitFixture repo;
  repo.init();
  repo.write("a.json", "1");
  const std::string c1 = repo.commit("one", "2016-03-04T05:06:07+00:00");
  repo.tag("light");
  repo.write("a.json", "2");
  const std::string c2 = repo.commit("two", "2016-03-05T05:06:07+00:00");
  repo.tag("annotated/v1.0", "a release");

  auto reader = GitReader::open(config_for(repo));
  ASSERT_OK(reader);
  const auto tags = reader->tags();
  ASSERT_OK(tags);
  ASSERT_EQ(tags->size(), 2u);
  EXPECT_EQ((*tags)[0].name, "annotated/v1.0");
  EXPECT_EQ((*tags)[0].commit, c2);
  EXPECT_EQ((*tags)[1].name, "light");
  EXPECT_EQ((*tags)[1].commit, c1);
}

TEST(GitReader, BranchWinsOverATagOfTheSameName) {
  SKIP_WITHOUT_GIT();
  GitFixture repo;
  repo.init();
  repo.write("a.json", "1");
  repo.commit("one", "2016-03-04T05:06:07+00:00");
  repo.tag("main");
  repo.write("a.json", "2");
  const std::string c2 = repo.commit("two", "2016-03-05T05:06:07+00:00");
  auto reader = GitReader::open(config_for(repo));
  ASSERT_OK(reader);
  EXPECT_EQ(reader->head(), c2);
}

TEST(GitReader, NoTags) {
  SKIP_WITHOUT_GIT();
  GitFixture repo;
  repo.init();
  repo.write("a.json", "1");
  repo.commit("one", "2016-03-04T05:06:07+00:00");
  auto reader = GitReader::open(config_for(repo));
  ASSERT_OK(reader);
  const auto tags = reader->tags();
  ASSERT_OK(tags);
  EXPECT_TRUE(tags->empty());
}

TEST(GitReader, IsAncestor) {
  SKIP_WITHOUT_GIT();
  GitFixture repo;
  repo.init();
  repo.write("a.json", "1");
  const std::string c1 = repo.commit("one", "2016-03-04T05:06:07+00:00");
  repo.branch("side");
  repo.write("a.json", "2");
  const std::string c2 = repo.commit("two", "2016-03-05T05:06:07+00:00");
  repo.checkout("side");
  repo.write("s.json", "1");
  const std::string side = repo.commit("side", "2016-03-06T05:06:07+00:00");
  repo.checkout("main");

  auto reader = GitReader::open(config_for(repo));
  ASSERT_OK(reader);
  EXPECT_TRUE(reader->is_ancestor(c1, c2).value_or(false));
  EXPECT_FALSE(reader->is_ancestor(c2, c1).value_or(true));
  EXPECT_TRUE(reader->is_ancestor(c1, c1).value_or(false));
  EXPECT_TRUE(reader->is_ancestor(c1, side).value_or(false));
  EXPECT_FALSE(reader->is_ancestor(side, c2).value_or(true));
  EXPECT_FALSE(reader->is_ancestor(std::string(40, 'd'), c2).has_value());  // no such commit
}

TEST(GitReader, DefaultBranch) {
  SKIP_WITHOUT_GIT();
  GitFixture repo;
  repo.init();
  repo.write("a.json", "1");
  repo.commit("one", "2016-03-04T05:06:07+00:00");
  repo.branch("work");

  GitConfig config = config_for(repo);
  config.branch = "work";
  auto reader = GitReader::open(config);
  ASSERT_OK(reader);
  const auto name = reader->default_branch();
  ASSERT_OK(name);
  EXPECT_EQ(*name, "main");
}

void expect_open_fails(const GitConfig& config, std::string_view reason) {
  const auto reader = GitReader::open(config);
  ASSERT_FALSE(reader.has_value());
  EXPECT_NE(reader.error().what.find(config.repo.string()), std::string::npos) << reader.error().what;
  EXPECT_NE(reader.error().what.find(reason), std::string::npos) << reader.error().what;
}

TEST(GitReader, OpenRejectsNonRepo) {
  SKIP_WITHOUT_GIT();
  GitFixture repo;
  GitConfig config = config_for(repo);
  config.repo = repo.temp("plain");
  std::filesystem::create_directories(config.repo);
  expect_open_fails(config, "not a git repository");

  config.repo = repo.temp("absent");
  expect_open_fails(config, "not a git repository");
}

TEST(GitReader, OpenRejectsADirectoryInsideARepo) {
  SKIP_WITHOUT_GIT();
  GitFixture repo;
  repo.init();
  repo.write("660/a.json", "1");
  repo.commit("one", "2016-03-04T05:06:07+00:00");
  GitConfig config = config_for(repo);
  config.repo = repo.path() / "660";
  expect_open_fails(config, "not a git repository");
}

TEST(GitReader, OpenRejectsMissingBranch) {
  SKIP_WITHOUT_GIT();
  GitFixture repo;
  repo.init();
  repo.write("a.json", "1");
  repo.commit("one", "2016-03-04T05:06:07+00:00");
  GitConfig config = config_for(repo);
  config.branch = "develop";
  expect_open_fails(config, "branch 'develop' not found");
  config.branch = "--all";
  expect_open_fails(config, "branch '--all' not found");
  config.branch = "";
  expect_open_fails(config, "no branch given");
}

TEST(GitReader, OpenRejectsEmptyRepo) {
  SKIP_WITHOUT_GIT();
  GitFixture repo;
  repo.init();
  expect_open_fails(config_for(repo), "no commits");
}

TEST(GitReader, OpenRejectsShallowClone) {
  SKIP_WITHOUT_GIT();
  GitFixture repo;
  repo.init();
  repo.write("a.json", "1");
  repo.commit("one", "2016-03-04T05:06:07+00:00");
  repo.write("a.json", "2");
  repo.commit("two", "2016-03-05T05:06:07+00:00");
  const std::filesystem::path shallow = repo.temp("shallow");
  repo.git({"clone", "--quiet", "--depth", "1", "--template=", repo.url(), shallow.string()});

  GitConfig config = config_for(repo);
  config.repo = shallow;
  expect_open_fails(config, "shallow");
}

TEST(GitReader, ReadsABareMirrorAndUpdatesIt) {
  SKIP_WITHOUT_GIT();
  GitFixture repo;
  repo.init();
  repo.write("a.json", "1");
  const std::string c1 = repo.commit("one", "2016-03-04T05:06:07+00:00");
  repo.tag("v1");
  const std::filesystem::path cache = repo.temp("mirrors");

  const auto first = GitReader::mirror(repo.url(), cache);
  ASSERT_OK(first);
  EXPECT_EQ(first->parent_path(), cache);
  EXPECT_TRUE(std::filesystem::is_directory(*first));

  GitConfig config = config_for(repo);
  config.repo = *first;
  {
    auto reader = GitReader::open(config);
    ASSERT_OK(reader);
    EXPECT_EQ(reader->head(), c1);
    EXPECT_EQ(*reader->default_branch(), "main");
    ASSERT_EQ(reader->tags()->size(), 1u);
  }

  // The source moves on, and loses its tag: the same directory follows.
  repo.write("a.json", "2");
  const std::string c2 = repo.commit("two", "2016-03-05T05:06:07+00:00");
  repo.git({"tag", "-d", "v1"});
  const auto second = GitReader::mirror(repo.url(), cache);
  ASSERT_OK(second);
  EXPECT_EQ(*second, *first);
  auto reader = GitReader::open(config);
  ASSERT_OK(reader);
  EXPECT_EQ(reader->head(), c2);
  EXPECT_EQ(*reader->rev_list(c1), (std::vector<std::string>{c2}));
  EXPECT_TRUE(reader->tags()->empty());
}

TEST(GitReader, MirrorNamesAreStableAndDistinct) {
  SKIP_WITHOUT_GIT();
  GitFixture one;
  one.init();
  one.write("a.json", "1");
  one.commit("one", "2016-03-04T05:06:07+00:00");
  const std::filesystem::path cache = one.temp("mirrors");
  const std::filesystem::path other_repo = one.temp("other/repo");
  one.git({"clone", "--quiet", "--template=", one.url(), other_repo.string()});
  const std::string other_url = "file://" + std::string(other_repo.generic_string().starts_with('/') ? "" : "/") +
                                other_repo.generic_string();

  const auto a = GitReader::mirror(one.url(), cache);
  const auto b = GitReader::mirror(other_url, cache);
  ASSERT_OK(a);
  ASSERT_OK(b);
  EXPECT_NE(*a, *b);  // both end in "repo"
  const std::string name = a->filename().string();
  EXPECT_TRUE(name.starts_with("repo-")) << name;
  EXPECT_TRUE(name.ends_with(".git")) << name;

  const auto bad = GitReader::mirror(one.url() + "-absent", cache);
  ASSERT_FALSE(bad.has_value());
  EXPECT_NE(bad.error().what.find("-absent"), std::string::npos) << bad.error().what;
  EXPECT_FALSE(GitReader::mirror("", cache).has_value());
}

}  // namespace
}  // namespace pychron::dvc
