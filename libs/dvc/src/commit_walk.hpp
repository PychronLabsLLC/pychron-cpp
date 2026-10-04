#pragma once

// What every adapter that walks a git history shares (legacy ingestion spec,
// section 10, items 12, 13, 17 and 20): the order commits are walked in, what
// a commit changes when it is a merge, and the resume token.
//
// The walk order is `git rev-list --topo-order --reverse` of the head: every
// commit, parents before children. A commit is diffed against its first
// parent; a merge is also diffed against each other parent, so that a file
// the merge took from its first parent, and that the other side had changed,
// is brought back to what the merge tree holds.
//
// The token of a batch is its last commit: "<sha>@<index>~<hash>", the
// commit, its place in the walk order and a hash of the commits up to it,
// with "+end" when the walk reached the head.

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/dvc/git_reader.hpp"

namespace pychron::dvc::detail {

// The token of a walk that has done commits [0, index] of `order`. `end`: it
// reached the head.
std::string format_token(const std::vector<std::string>& order, std::size_t index, bool end);

struct ResumePoint {
  std::size_t first = 0;  // the first commit of the order to walk
  bool at_end = false;    // the token was written by the last batch of a walk
};

// Where a walk of `order` continues after `token` (nullopt or empty: at the
// start). Same commit at the token's index after the same commits: after it.
// Commit in the order at another place or after other commits (the order
// changed), or a token in an older format: at the start; ids are derived from
// commit and path, so what is stored is skipped. Commit not in the order: the
// history was rewritten, an error whose message contains "history was
// rewritten". Text that is no token: an error naming `adapter`.
Result<ResumePoint> resume_point(const std::vector<std::string>& order, const std::optional<std::string>& token,
                                 const GitReader& reader, const GitConfig& git, std::string_view adapter);

// The place of every commit of a walk order (ISourceAdapter::order_of).
using Places = std::unordered_map<std::string, std::int64_t>;
Places places(const std::vector<std::string>& order);
std::optional<std::int64_t> place_of(const Places& places, std::string_view commit);

// Lists the commits [begin, end) of `order` into `commits` (in that order)
// and hands the changes of each to `apply`, oldest first: what differs from
// its first parent and, for a merge, what the merge tree holds that another
// parent did not, each path once. A caller that skips every path which
// already has the blob it is given is left with the files the merge moved
// away from the content last seen. `apply` returns true to stop after the
// commit it was given. Returns one past the last commit applied.
Result<std::size_t> walk_commits(
    const GitReader& reader, const std::vector<std::string>& order, std::size_t begin, std::size_t end,
    std::vector<GitCommit>& commits,
    const std::function<bool(std::size_t index, std::span<const GitChange> changes)>& apply);

// The exception boundary of an adapter: what its walk throws (the standard
// library, the JSON library, a callback of the caller's) comes back as an
// error naming the adapter, as every other failure does. A fault of one
// file's content never gets here: the parsers return it as an error, which
// is recorded as a conflict of that file.
template <class Fn>
auto contained(std::string_view adapter, Fn&& fn) -> decltype(fn()) {
  try {
    return fn();
  } catch (const std::exception& e) {
    return fail(ErrorKind::Io, std::string(adapter) + ": " + e.what());
  }
}

}  // namespace pychron::dvc::detail
