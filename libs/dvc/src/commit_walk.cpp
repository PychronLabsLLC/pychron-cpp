// The commit order, merge changes and resume token shared by the adapters
// (commit_walk.hpp).

#include "commit_walk.hpp"

#include <algorithm>
#include <charconv>
#include <unordered_set>
#include <utility>

#include "pychron/core/path_text.hpp"
#include "pychron/core/sha256.hpp"

namespace pychron::dvc::detail {

namespace {

constexpr std::string_view kEnd = "+end";
constexpr std::size_t kHashDigits = 16;

struct Token {
  std::string sha;
  std::optional<std::size_t> index;  // nullopt: an older format without one
  std::string hash;                  // of the commits up to and including `sha`; empty: an older format
  bool end = false;                  // written by the last batch of a walk
};

bool is_sha(std::string_view text) {
  if (text.size() != 40 && text.size() != 64) return false;
  return std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// A hash of the first `count` commits of the walk order.
std::string prefix_hash(const std::vector<std::string>& order, std::size_t count) {
  Sha256 hasher;
  for (std::size_t i = 0; i < count; ++i) {
    hasher.update(order[i]);
    hasher.update(std::string_view("\n"));
  }
  const Sha256Digest digest = hasher.finish();
  return to_hex(digest).substr(0, kHashDigits);
}

// nullopt: not a token an adapter wrote.
std::optional<Token> parse_token(std::string_view text) {
  Token token;
  const auto at = text.find('@');
  token.sha = std::string(text.substr(0, at));
  if (!is_sha(token.sha)) return std::nullopt;
  if (at == std::string_view::npos) return token;  // a bare sha: a commit without a place
  std::string_view rest = text.substr(at + 1);
  if (rest.ends_with(kEnd)) {
    token.end = true;
    rest.remove_suffix(kEnd.size());
  }
  if (const auto tilde = rest.find('~'); tilde != std::string_view::npos) {
    token.hash = std::string(rest.substr(tilde + 1));
    if (token.hash.empty()) return std::nullopt;
    rest = rest.substr(0, tilde);
  }
  std::size_t index = 0;
  const auto [last, error] = std::from_chars(rest.data(), rest.data() + rest.size(), index);
  if (error != std::errc{} || last != rest.data() + rest.size() || rest.empty()) return std::nullopt;
  token.index = index;
  return token;
}

}  // namespace

std::string format_token(const std::vector<std::string>& order, std::size_t index, bool end) {
  return order[index] + "@" + std::to_string(index) + "~" + prefix_hash(order, index + 1) +
         (end ? std::string(kEnd) : std::string());
}

Result<ResumePoint> resume_point(const std::vector<std::string>& order, const std::optional<std::string>& token,
                                 const GitReader& reader, const GitConfig& git, std::string_view adapter) {
  ResumePoint point;
  if (!token || token->empty()) return point;
  const auto parsed = parse_token(*token);
  if (!parsed) return fail(ErrorKind::Config, std::string(adapter) + ": '" + *token + "' is not a resume token");
  if (parsed->index && !parsed->hash.empty() && *parsed->index < order.size() &&
      order[*parsed->index] == parsed->sha && prefix_hash(order, *parsed->index + 1) == parsed->hash) {
    point.first = *parsed->index + 1;
    point.at_end = parsed->end;
  } else if (std::find(order.begin(), order.end(), parsed->sha) == order.end()) {
    // Every commit of the branch is in the list: the token's is not an
    // ancestor of the head any more, or was never in this repository.
    return fail(ErrorKind::Protocol, "git repository " + utf8(git.repo) + ": resume token commit " + parsed->sha +
                                         " is not in the history of " + git.branch + " (" + reader.head() +
                                         "); history was rewritten");
  }
  // Otherwise the commit is in the history at another place or after other
  // commits (the order changed), or the token is in an older format: the
  // walk starts again from the first commit.
  return point;
}

Places places(const std::vector<std::string>& order) {
  Places out;
  out.reserve(order.size());
  for (std::size_t i = 0; i < order.size(); ++i) out.emplace(order[i], static_cast<std::int64_t>(i));
  return out;
}

std::optional<std::int64_t> place_of(const Places& places, std::string_view commit) {
  const auto found = places.find(std::string(commit));
  if (found == places.end()) return std::nullopt;
  return found->second;
}

Result<std::size_t> walk_commits(
    const GitReader& reader, const std::vector<std::string>& order, std::size_t begin, std::size_t end,
    std::vector<GitCommit>& commits,
    const std::function<bool(std::size_t index, std::span<const GitChange> changes)>& apply) {
  const std::span<const std::string> slice(order.data() + begin, end - begin);
  auto listed = reader.commits(slice);  // in the order asked
  if (!listed) return fail(listed.error());
  commits = std::move(*listed);
  auto changes = reader.changes(slice);
  if (!changes) return fail(changes.error());
  // Grouped by commit in the order asked; a commit that changes nothing is absent.
  std::size_t at = 0;
  for (std::size_t i = begin; i < end; ++i) {
    std::size_t stop = at;
    while (stop < changes->size() && (*changes)[stop].commit == order[i]) ++stop;
    const std::span<const GitChange> of_commit(changes->data() + at, stop - at);
    at = stop;
    const auto& parents = commits[i - begin].parents;
    if (parents.size() < 2) {
      if (apply(i, of_commit)) return i + 1;
      continue;
    }
    // A merge: what differs from its first parent, then, for each other
    // parent, what the merge tree holds that this parent did not.
    std::vector<GitChange> merged(of_commit.begin(), of_commit.end());
    std::unordered_set<std::string> seen;
    for (const auto& entry : merged) seen.insert(entry.path);
    for (std::size_t p = 1; p < parents.size(); ++p) {
      auto against = reader.diff(parents[p], order[i]);
      if (!against) return fail(against.error());
      for (auto& entry : *against)
        if (seen.insert(entry.path).second) merged.push_back(std::move(entry));
    }
    if (apply(i, merged)) return i + 1;
  }
  if (at != changes->size())
    return fail(ErrorKind::Protocol, "git: changes of commit " + (*changes)[at].commit + " were not asked for");
  return end;
}

}  // namespace pychron::dvc::detail
