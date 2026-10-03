// ProjectRepoAdapter: the commit list, the batches cut from it, the resume
// token. What a commit means is decided in project_walk.cpp, what a file
// holds in project_map.cpp.
//
// The walk order is `git rev-list --topo-order --reverse` of the head: every
// commit, parents before children. A commit is diffed against its first
// parent; a merge is also diffed against each other parent, so that a file
// the merge took from its first parent, and that the other side had changed,
// is brought back to what the merge tree holds.
//
// Batches. A batch is batch_commits commits of the walk order. It ends
// earlier after a tagged commit (the bookmark captures the heads as they are
// once the batch is written) and after a commit that rewrites the record of
// an analysis already folded (the rewrite can change a run identity, and the
// writer stores a batch's analyses before its changesets: a later analysis
// that takes the freed run id must come in a later batch). Where the cuts
// fall does not change what is stored.
//
// Resume. The token of a batch is its last commit: "<sha>@<index>~<hash>",
// the commit, its place in the walk order and a hash of the commits up to it,
// with "+end" when the walk reached the head. plan() computes the order
// again. Same commit at that index after the same commits: the walk continues
// after it. The commits before it are first replayed through the Walk without
// reading a file, which rebuilds what the earlier run knew: which file
// belongs to which analysis, the blob each path was left with, which
// collections were folded. An analysis still pending at the token stays
// pending and is folded by the resumed walk, as an uninterrupted walk would.
// Commit in the history but not there, or after other commits (the order
// changed), or a token in an older format: the walk starts again from the
// first commit; ids are derived from commit and path, so what is stored is
// skipped. Commit not in the history: the history was rewritten, an error.
//
// At the end of the walk the analyses still pending are folded with what they
// have. A later run that resumes from an "+end" token treats them as folded.

#include "pychron/dvc/project_adapter.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <unordered_set>
#include <utility>

#include "project_import.hpp"
#include "pychron/core/sha256.hpp"
#include "pychron/ingest/ids.hpp"

namespace pychron::dvc {

namespace {

// Commits whose changes are listed in one git call while replaying.
constexpr std::size_t kReplayChunk = 2000;
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

std::string format_token(const std::vector<std::string>& order, std::size_t index, bool end) {
  return order[index] + "@" + std::to_string(index) + "~" + prefix_hash(order, index + 1) +
         (end ? std::string(kEnd) : std::string());
}

// nullopt: not a token this adapter wrote.
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

class ProjectRepoAdapter::Impl {
 public:
  Impl(ProjectAdapterConfig config, GitReader reader)
      : config_(std::move(config)), reader_(std::move(reader)), url_(ingest::normalize_source_url(config_.url)) {
    if (config_.batch_commits < 1) config_.batch_commits = 1;
  }

  Result<ingest::SourceDescription> describe() const {
    return ingest::SourceDescription{persistence::ImportSourceKind::ProjectRepo, config_.url, config_.git.branch,
                                     reader_.head()};
  }

  Result<int> plan(std::optional<std::string> resume_token, ingest::IImportState& state) {
    walk_ = detail::Walk{config_.collection_wait_commits};
    mapper_.emplace(config_, url_, reader_, walk_, state);
    order_.clear();
    tags_.clear();
    finished_ = planned_ = false;
    first_ = next_ = 0;

    auto all = reader_.rev_list(std::nullopt);
    if (!all) return fail(all.error());
    order_ = std::move(*all);

    bool resumed_at_end = false;
    if (resume_token && !resume_token->empty()) {
      const auto token = parse_token(*resume_token);
      if (!token) return fail(ErrorKind::Config, "project adapter: '" + *resume_token + "' is not a resume token");
      if (token->index && !token->hash.empty() && *token->index < order_.size() &&
          order_[*token->index] == token->sha && prefix_hash(order_, *token->index + 1) == token->hash) {
        first_ = *token->index + 1;
        resumed_at_end = token->end;
      } else if (std::find(order_.begin(), order_.end(), token->sha) == order_.end()) {
        // Every commit of the branch is in the list: the token's is not an
        // ancestor of the head any more, or was never in this repository.
        return fail(ErrorKind::Protocol, "git repository " + config_.git.repo.string() + ": resume token commit " +
                                             token->sha + " is not in the history of " + config_.git.branch + " (" +
                                             reader_.head() + "); history was rewritten");
      }
      // Otherwise the commit is in the history at another place or after
      // other commits (the order changed), or the token is in an older
      // format: the walk starts again from the first commit.
    }

    planned_ = true;
    next_ = first_;
    if (first_ == order_.size()) return 0;  // nothing new: no batch will be asked for

    for (std::size_t begin = 0; begin < first_; begin += kReplayChunk)
      if (auto r = walk(begin, std::min(first_, begin + kReplayChunk), nullptr); !r) return fail(r.error());
    if (resumed_at_end) walk_.assume_written();

    auto tags = reader_.tags();
    if (!tags) return fail(tags.error());
    tags_ = std::move(*tags);
    return static_cast<int>(order_.size() - first_);
  }

  Result<std::optional<ingest::ImportBatch>> next_batch() {
    if (!planned_) return fail(ErrorKind::Config, "project adapter: next_batch() before plan()");
    if (finished_ || next_ == order_.size()) return std::optional<ingest::ImportBatch>{};

    ingest::ImportBatch batch;
    batch.head = reader_.head();
    batch.total = static_cast<int>(order_.size());
    std::vector<detail::Work> work;

    std::size_t end = std::min(order_.size(), next_ + static_cast<std::size_t>(config_.batch_commits));
    for (std::size_t i = next_; i < end; ++i)
      if (tagged(order_[i])) end = i + 1;
    auto walked = walk(next_, end, &work);
    if (!walked) return fail(walked.error());
    next_ = *walked;
    if (next_ == order_.size()) {
      // The end of the walk: what is still pending is folded with what it has.
      for (detail::Track* track : walk_.pending()) walk_.force(*track, work);
      walk_.orphans(work);
      finished_ = true;
    }

    if (auto r = mapper_->map(work, batch); !r) return fail(r.error());
    for (const auto& tag : tags_)
      if (tag.commit == order_[next_ - 1])
        if (auto r = mapper_->bookmark(tag, batch); !r) return fail(r.error());

    // Every batch moves the token: what is pending at it is rebuilt on resume.
    batch.done = static_cast<int>(next_);
    batch.resume_token = format_token(order_, next_ - 1, finished_);
    return std::optional<ingest::ImportBatch>{std::move(batch)};
  }

 private:
  bool tagged(const std::string& sha) const {
    return std::any_of(tags_.begin(), tags_.end(), [&](const GitTag& tag) { return tag.commit == sha; });
  }

  // Applies commits [begin, end) of the walk order and returns one past the
  // last commit applied: `end`, or earlier where a batch must end (Walk::apply).
  // `out` null: a replay, which applies them all.
  Result<std::size_t> walk(std::size_t begin, std::size_t end, std::vector<detail::Work>* out) {
    const std::span<const std::string> slice(order_.data() + begin, end - begin);
    auto commits = reader_.commits(slice);  // in the order asked
    if (!commits) return fail(commits.error());
    auto changes = reader_.changes(slice);
    if (!changes) return fail(changes.error());
    // Grouped by commit in the order asked; a commit that changes nothing is absent.
    std::size_t at = 0;
    for (std::size_t i = begin; i < end; ++i) {
      std::size_t stop = at;
      while (stop < changes->size() && (*changes)[stop].commit == order_[i]) ++stop;
      std::span<const GitChange> of_commit(changes->data() + at, stop - at);
      at = stop;
      const auto& parents = (*commits)[i - begin].parents;
      if (parents.size() < 2) {
        if (walk_.apply(static_cast<int>(i), of_commit, out) && out) return cut(i + 1, std::move(*commits));
        continue;
      }
      // A merge: what differs from its first parent, then, for each other
      // parent, what the merge tree holds that this parent did not. The Walk
      // skips every path that already has the merge's blob, so what is left
      // are the files the merge moved away from the content last imported.
      std::vector<GitChange> merged(of_commit.begin(), of_commit.end());
      std::unordered_set<std::string> listed;
      for (const auto& entry : merged) listed.insert(entry.path);
      for (std::size_t p = 1; p < parents.size(); ++p) {
        auto against = reader_.diff(parents[p], order_[i]);
        if (!against) return fail(against.error());
        for (auto& entry : *against)
          if (listed.insert(entry.path).second) merged.push_back(std::move(entry));
      }
      if (walk_.apply(static_cast<int>(i), merged, out) && out) return cut(i + 1, std::move(*commits));
    }
    if (out) mapper_->remember(std::move(*commits));
    if (at != changes->size())
      return fail(ErrorKind::Protocol, "git: changes of commit " + (*changes)[at].commit + " were not asked for");
    return end;
  }

  // The batch ends before `end`: the commits already listed are still remembered.
  std::size_t cut(std::size_t end, std::vector<GitCommit> commits) {
    mapper_->remember(std::move(commits));
    return end;
  }

  ProjectAdapterConfig config_;
  GitReader reader_;
  std::string url_;  // normalized

  bool planned_ = false;
  std::vector<std::string> order_;  // commits earlier runs walked, then those to walk
  std::size_t first_ = 0;           // the first commit to walk
  std::size_t next_ = 0;
  std::vector<GitTag> tags_;
  detail::Walk walk_;
  std::optional<detail::Mapper> mapper_;
  bool finished_ = false;               // the last batch is built
};

ProjectRepoAdapter::ProjectRepoAdapter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ProjectRepoAdapter::~ProjectRepoAdapter() = default;

Result<std::unique_ptr<ProjectRepoAdapter>> ProjectRepoAdapter::open(ProjectAdapterConfig config) {
  if (config.repository_name.empty()) return fail(ErrorKind::Config, "project adapter: no repository name");
  if (config.url.empty()) return fail(ErrorKind::Config, "project adapter: no source url");
  auto reader = GitReader::open(config.git);
  if (!reader) return fail(reader.error());
  return std::unique_ptr<ProjectRepoAdapter>(
      new ProjectRepoAdapter(std::make_unique<Impl>(std::move(config), std::move(*reader))));
}

Result<ingest::SourceDescription> ProjectRepoAdapter::describe() { return impl_->describe(); }

Result<int> ProjectRepoAdapter::plan(std::optional<std::string> resume_token, ingest::IImportState& state) {
  return impl_->plan(std::move(resume_token), state);
}

Result<std::optional<ingest::ImportBatch>> ProjectRepoAdapter::next_batch() { return impl_->next_batch(); }

}  // namespace pychron::dvc
