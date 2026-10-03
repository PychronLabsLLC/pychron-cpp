#pragma once

// GitFixture: a throwaway git repository for tests of GitReader and of the
// adapters built on it.
//
//   if (!GitFixture::available()) GTEST_SKIP() << "git not on PATH";
//   GitFixture repo;
//   repo.init();                                   // branch "main", no commits yet
//   repo.write("660/52-01E.json", "{}");           // parent directories are created
//   const std::string c1 = repo.commit("add", "2016-03-04T05:06:07-07:00");
//   repo.branch("side"); repo.checkout("side"); ...
//   repo.checkout("main"); repo.merge("side", "merge side", "2016-03-06T00:00:00+00:00");
//
// Layout, all under one fresh directory of std::filesystem::temp_directory_path()
// that the destructor removes:
//   <root>/repo       the work tree, path()
//   <root>/gitconfig  an empty file used as the global git configuration
//   <root>/...        temp(name): room for clones, mirrors and scratch files
//
// It is deterministic. Every command runs as `git -C <root>/repo` with the
// system configuration off, the empty global configuration, discovery capped
// at <root> (so it can never reach a repository above the temp directory),
// no hooks, no signing, no line-ending conversion and no rename detection in
// the user's hands. Author name, email and date come from the arguments of
// commit() and merge(); the committer is always "Fixture <fixture@example.org>"
// with the same date. The same calls therefore give the same commit shas on
// every machine.
//
// commit() stages everything (`git add -A`) and allows an empty commit.
// merge() always makes a merge commit (--no-ff); its second parent is `name`.
// A failing git command throws std::runtime_error with git's output, which
// fails the test. Paths are UTF-8 and relative to the work tree.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pychron/core/process.hpp"

namespace pychron::dvc::testing {

class GitFixture {
 public:
  static bool available() {
    ProcessSpec spec;
    spec.argv = {"git", "--version"};
    const auto result = run_process(spec);
    return result && result->exit_code == 0;
  }

  GitFixture() {
    std::random_device device;
    std::ostringstream name;
    name << "pychron-git-fixture-" << std::hex << device() << device();
    root_ = std::filesystem::temp_directory_path() / name.str();
    std::filesystem::create_directories(root_);
    root_ = std::filesystem::canonical(root_);
    std::ofstream(root_ / "gitconfig", std::ios::binary).flush();
  }

  ~GitFixture() {
    std::error_code ignored;
    // Git marks object files read-only; Windows will not delete those.
    std::filesystem::permissions(root_, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::add, ignored);
    for (std::filesystem::recursive_directory_iterator it(root_, ignored), end; !ignored && it != end;
         it.increment(ignored)) {
      std::error_code each;
      std::filesystem::permissions(it->path(), std::filesystem::perms::owner_all,
                                   std::filesystem::perm_options::add, each);
    }
    std::filesystem::remove_all(root_, ignored);
  }

  GitFixture(const GitFixture&) = delete;
  GitFixture& operator=(const GitFixture&) = delete;

  // An empty repository whose unborn branch is "main".
  void init() {
    std::filesystem::create_directories(path());
    run_in({}, {"init", "--quiet", "--initial-branch=main", "--template=", path().string()});
  }

  void write(std::string_view relative, std::string_view text) {
    const std::filesystem::path file = path() / utf8(relative);
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) throw std::runtime_error("GitFixture: cannot write " + std::string(relative));
  }

  void remove(std::string_view relative) { std::filesystem::remove(path() / utf8(relative)); }

  // Returns the new commit's sha. `date_iso` is "YYYY-MM-DDTHH:MM:SS+HH:MM".
  std::string commit(std::string_view message, std::string_view date_iso,
                     std::string_view author = "Ann <ann@example.org>") {
    git({"add", "-A"});
    git({"commit", "--quiet", "--allow-empty", "--cleanup=verbatim", "-m", std::string(message)}, date_iso,
        author);
    return head();
  }

  void branch(std::string_view name) { git({"branch", std::string(name)}); }
  void checkout(std::string_view name) { git({"checkout", "--quiet", std::string(name)}); }

  std::string merge(std::string_view name, std::string_view message, std::string_view date_iso) {
    git({"merge", "--quiet", "--no-ff", "--no-edit", "-m", std::string(message), std::string(name)}, date_iso);
    return head();
  }

  // A lightweight tag on HEAD; with a message, an annotated one.
  void tag(std::string_view name, std::optional<std::string> annotation = std::nullopt) {
    if (annotation)
      git({"tag", "-a", "-m", *annotation, std::string(name)}, "2000-01-01T00:00:00+00:00");
    else
      git({"tag", std::string(name)});
  }

  std::string head() { return trimmed(git({"rev-parse", "HEAD"})); }

  // `git -C path() <args>`; returns its output (stdout and stderr together).
  std::string git(std::vector<std::string> args, std::string_view date_iso = "2000-01-01T00:00:00+00:00",
                  std::string_view author = "Ann <ann@example.org>") {
    return run_in(path(), std::move(args), date_iso, author);
  }

  const std::filesystem::path& root() const { return root_; }
  std::filesystem::path path() const { return root_ / "repo"; }
  // A path beside the repository, removed with the fixture; not created.
  std::filesystem::path temp(std::string_view name) const { return root_ / utf8(name); }
  // path() as a file:// URL, for clone and mirror.
  std::string url() const {
    const std::string generic = path().generic_string();
    return (generic.starts_with('/') ? "file://" : "file:///") + generic;  // file:///C:/... on Windows
  }

 private:
  static std::filesystem::path utf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
  }

  static std::string trimmed(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return text;
  }

  std::string run_in(const std::filesystem::path& dir, std::vector<std::string> args,
                     std::string_view date_iso = "2000-01-01T00:00:00+00:00",
                     std::string_view author = "Ann <ann@example.org>") {
    const auto open = author.find(" <");
    const auto close = author.rfind('>');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open)
      throw std::runtime_error("GitFixture: author must be \"Name <email>\"");

    ProcessSpec spec;
    spec.argv = {"git"};
    if (!dir.empty()) {
      spec.argv.push_back("-C");
      spec.argv.push_back(dir.string());
    }
    for (const char* option :
         {"core.hooksPath=no-hooks", "core.autocrlf=false", "core.quotepath=false", "core.fsmonitor=false",
          "core.precomposeUnicode=false", "commit.gpgsign=false", "tag.gpgsign=false", "tag.forceSignAnnotated=false",
          "init.defaultBranch=main", "merge.ff=false", "gc.auto=0", "advice.detachedHead=false",
          "protocol.file.allow=always"}) {
      spec.argv.push_back("-c");
      spec.argv.push_back(option);
    }
    for (auto& arg : args) spec.argv.push_back(std::move(arg));
    spec.env = {
        {"GIT_CONFIG_NOSYSTEM", "1"},
        {"GIT_CONFIG_GLOBAL", (root_ / "gitconfig").string()},
        {"GIT_CEILING_DIRECTORIES", root_.string()},
        {"GIT_TERMINAL_PROMPT", "0"},
        {"LC_ALL", "C"},
        {"GIT_AUTHOR_NAME", std::string(author.substr(0, open))},
        {"GIT_AUTHOR_EMAIL", std::string(author.substr(open + 2, close - open - 2))},
        {"GIT_AUTHOR_DATE", std::string(date_iso)},
        {"GIT_COMMITTER_NAME", "Fixture"},
        {"GIT_COMMITTER_EMAIL", "fixture@example.org"},
        {"GIT_COMMITTER_DATE", std::string(date_iso)},
    };
    spec.timeout = std::chrono::seconds(60);
    const auto result = run_process(spec);
    if (!result) throw std::runtime_error("GitFixture: cannot run git: " + result.error().what);
    if (result->exit_code != 0) {
      std::string line = "GitFixture:";
      for (const auto& arg : spec.argv) line += " " + arg;
      throw std::runtime_error(line + " failed: " + result->output);
    }
    return result->output;
  }

  std::filesystem::path root_;
};

}  // namespace pychron::dvc::testing
