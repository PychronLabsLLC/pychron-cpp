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
// It cannot touch any repository but its own. Every command runs with
// GIT_DIR, GIT_WORK_TREE, GIT_INDEX_FILE, GIT_OBJECT_DIRECTORY and
// GIT_COMMON_DIR set to the fixture's repository (or, for clone_bare(), to the
// clone being made), so git discovers nothing and a value inherited from the
// process that started the tests (a git hook exports them) is replaced, not
// obeyed.
//
// It is deterministic: system configuration off, the empty file as global
// configuration (an empty file rather than the null device, so the same line
// works on every platform), no hooks, no signing, no line-ending conversion. Author name, email and date come from the arguments of
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
#include <cstdlib>
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
  // From here on no git child of this process reads the machine's or the
  // user's git configuration. The fixture's own commands never did; this is
  // for the code under test that leaves the choice to its environment
  // (GitReader::mirror uses the user's configuration to reach a remote), so
  // that a test does not depend on the gitconfig of whoever runs it. Called
  // by the constructor; a test that wants a configuration sets
  // GIT_CONFIG_GLOBAL itself afterwards.
  static void ignore_user_git_configuration() {
#ifdef _WIN32
    _putenv_s("GIT_CONFIG_NOSYSTEM", "1");
    _putenv_s("GIT_CONFIG_GLOBAL", "NUL");
#else
    ::setenv("GIT_CONFIG_NOSYSTEM", "1", 1);
    ::setenv("GIT_CONFIG_GLOBAL", "/dev/null", 1);
#endif
  }

  static bool available() {
    ProcessSpec spec;
    spec.argv = {"git", "--version"};
    const auto result = run_process(spec);
    return result && result->exit_code == 0;
  }

  GitFixture() {
    ignore_user_git_configuration();
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
    git({"init", "--quiet", "--initial-branch=main", "--template="});  // GIT_DIR says where
  }

  // A bare clone of this repository at temp(name), which it returns:
  // `git clone --bare <extra> <url()> <temp(name)>`.
  std::filesystem::path clone_bare(std::string_view name, std::vector<std::string> extra = {}) {
    const std::filesystem::path destination = temp(name);
    std::vector<std::string> args{"clone", "--quiet", "--bare", "--template="};
    for (auto& arg : extra) args.push_back(std::move(arg));
    args.push_back(url());
    args.push_back(destination.string());
    run(root_, destination, destination, std::move(args), kNoDate, kAnn);
    return destination;
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
  std::string git(std::vector<std::string> args, std::string_view date_iso = kNoDate,
                  std::string_view author = kAnn) {
    return run(path(), path() / ".git", path(), std::move(args), date_iso, author);
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
  static constexpr std::string_view kNoDate = "2000-01-01T00:00:00+00:00";
  static constexpr std::string_view kAnn = "Ann <ann@example.org>";

  static std::filesystem::path utf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
  }

  static std::string trimmed(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return text;
  }

  // git in directory `cwd`, on the repository at `git_dir` with work tree
  // `work_tree`, and on no other.
  std::string run(const std::filesystem::path& cwd, const std::filesystem::path& git_dir,
                  const std::filesystem::path& work_tree, std::vector<std::string> args, std::string_view date_iso,
                  std::string_view author) {
    const auto open = author.find(" <");
    const auto close = author.rfind('>');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open)
      throw std::runtime_error("GitFixture: author must be \"Name <email>\"");

    ProcessSpec spec;
    spec.argv = {"git", "-C", cwd.string()};
    for (const char* option :
         {"core.hooksPath=no-hooks", "core.autocrlf=false", "core.quotepath=false", "core.fsmonitor=false",
          "core.precomposeUnicode=false", "commit.gpgsign=false", "tag.gpgsign=false", "tag.forceSignAnnotated=false",
          "init.defaultBranch=main", "merge.ff=false", "gc.auto=0", "advice.detachedHead=false",
          "protocol.file.allow=always"}) {
      spec.argv.emplace_back("-c");
      spec.argv.emplace_back(option);
    }
    for (auto& arg : args) spec.argv.push_back(std::move(arg));
    spec.env = {
        {"GIT_CONFIG_NOSYSTEM", "1"},
        {"GIT_CONFIG_GLOBAL", (root_ / "gitconfig").string()},
        {"GIT_DIR", git_dir.string()},
        {"GIT_WORK_TREE", work_tree.string()},
        {"GIT_INDEX_FILE", (git_dir / "index").string()},
        {"GIT_OBJECT_DIRECTORY", (git_dir / "objects").string()},
        {"GIT_COMMON_DIR", git_dir.string()},
        {"GIT_ALTERNATE_OBJECT_DIRECTORIES", ""},
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
