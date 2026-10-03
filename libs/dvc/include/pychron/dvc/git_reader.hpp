#pragma once

// Reads a git repository's history without checking anything out (legacy
// ingestion spec, section 4): list the commits of one branch, diff each
// against its first parent, read blobs. Every call is one `git` subprocess
// that runs to completion; nothing here links a git library.
//
// Nothing writes to the repository being read. mirror() is the only function
// that writes at all, and only inside the cache directory it is given.
//
// The child environment is fixed: system and global configuration off, no
// terminal prompt, C locale, no replace objects, no optional locks. git never
// discovers the repository: GIT_DIR, GIT_COMMON_DIR, GIT_OBJECT_DIRECTORY,
// GIT_INDEX_FILE and GIT_WORK_TREE are set for every child from the path
// given, so values inherited from the parent process (a git hook exports
// them) cannot send a command to another repository. `repo` must therefore
// be the repository itself (a work tree, a linked work tree, or a
// bare/mirror directory), not a directory inside one.
//
// Needs git 2.32 or newer (diff-tree --diff-merges=first-parent is 2.31,
// GIT_CONFIG_GLOBAL is 2.32); open() checks.
//
// Shas are full lower-case hex object names (40 digits, or 64 in a SHA-256
// repository), as git prints them. Paths are the raw bytes git stores,
// '/'-separated, never quoted.

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/ingest/batch.hpp"

namespace pychron::dvc {

struct GitCommit {
  std::string sha;
  std::vector<std::string> parents;  // first parent first; empty for a root commit
  ingest::GitWho author;             // author date, converted to UTC
  // Subject and body as stored, less the one newline that ends the message;
  // further trailing newlines are kept.
  std::string message;
};

struct GitVersion {
  int major_number = 0, minor_number = 0, patch_number = 0;
  friend auto operator<=>(const GitVersion&, const GitVersion&) = default;
};

// The oldest git the reader works with.
inline constexpr GitVersion kMinimumGitVersion{2, 32, 0};

// The version in the output of `git --version`: "git version 2.43.0",
// "git version 2.50.1 (Apple Git-155)", "git version 2.39.2.windows.1". A
// missing patch number is 0. nullopt when the text is not that.
std::optional<GitVersion> parse_git_version(std::string_view text);

// One file that differs between a commit and its first parent.
struct GitChange {
  std::string commit, path;
  std::string blob_sha;  // the file's blob at `commit`; empty for a deletion
  char status = 'M';     // A, M, D; renames arrive as D + A
};

struct GitTag {
  std::string name;    // without "refs/tags/"
  std::string commit;  // an annotated tag is peeled to its commit
};

struct GitConfig {
  std::filesystem::path repo;
  std::string branch;               // a branch name; a tag of the same name does not shadow it
  std::filesystem::path scratch;    // for the output of git; created if missing; empty: the system temp directory
  std::chrono::milliseconds timeout{std::chrono::minutes(30)};  // per git command
  std::size_t cache_bytes = 256u << 20;
};

class GitReader {
 public:
  // Checks that git runs and is new enough, that `repo` is a repository, that it is not a
  // shallow clone, and that `branch` names a commit; that commit is head()
  // for the life of the reader, whatever happens to the branch afterwards.
  // Each failure names the repository path and the reason.
  static Result<GitReader> open(GitConfig config);

  // A bare mirror of `url` under `cache_dir`, in a directory whose name
  // depends only on the url: cloned (`git clone --mirror`) when absent,
  // otherwise updated (`git fetch --prune`, which also drops refs deleted at
  // the source). Returns the directory, ready for open().
  static Result<std::filesystem::path> mirror(std::string_view url, const std::filesystem::path& cache_dir,
                                              std::chrono::milliseconds timeout = std::chrono::minutes(30));

  // Movable, not copyable: the cache index points into the cache itself. A
  // moved reader keeps its head and its blobs.
  GitReader(GitReader&&) = default;
  GitReader& operator=(GitReader&&) = default;
  GitReader(const GitReader&) = delete;
  GitReader& operator=(const GitReader&) = delete;

  const std::string& head() const { return head_; }

  // The branch the repository's HEAD points at ("main"); an error when HEAD
  // is detached.
  Result<std::string> default_branch() const;

  // The commits reachable from head(), parents before children (topological
  // order, oldest first). With `after`, only those not reachable from it;
  // `after` must be an ancestor of head() (or head() itself: nothing new).
  // An `after` that is not, or that the repository does not have, is an
  // error: history was rewritten.
  Result<std::vector<std::string>> rev_list(std::optional<std::string> after) const;

  Result<bool> is_ancestor(std::string_view ancestor, std::string_view descendant) const;

  // One GitCommit per sha, in the order asked.
  Result<std::vector<GitCommit>> commits(std::span<const std::string> shas) const;

  // The changes of each commit against its first parent (a root commit
  // against the empty tree), grouped by commit in the order asked, paths in
  // git's order within a commit. A merge lists only what differs from its
  // first parent. A commit that changes nothing contributes nothing.
  // Submodule entries are skipped.
  Result<std::vector<GitChange>> changes(std::span<const std::string> shas) const;

  // Reads the blobs not already cached, in one git call, then evicts the
  // least recently used blobs not named in this call until the cache is
  // within cache_bytes. Every blob named in a call is therefore readable
  // until the next call, even when together they exceed the cache. A missing
  // object, or one that is not a blob, is an error and caches nothing.
  Result<void> fetch_blobs(std::span<const std::string> blob_shas);

  // The bytes of a fetched blob; the view is valid until the next
  // fetch_blobs(). A blob never fetched, or evicted since, is an error.
  Result<std::string_view> blob(const std::string& blob_sha);

  // Every tag that names a commit, sorted by name.
  Result<std::vector<GitTag>> tags() const;

 private:
  struct Blob {
    std::string sha, bytes;
  };

  GitReader(GitConfig config, std::filesystem::path git_dir, std::filesystem::path common_dir, std::string head);

  GitConfig config_;
  std::filesystem::path git_dir_, common_dir_;  // where `repo` keeps its own files and the shared ones
  std::string head_;
  std::list<Blob> lru_;  // most recently used first
  std::unordered_map<std::string, std::list<Blob>::iterator> index_;
  std::size_t cached_bytes_ = 0;
};

}  // namespace pychron::dvc
