// ProjectRepoAdapter: the commit list, the batches cut from it, the resume
// token. What a commit means is decided in project_walk.cpp, what a file
// holds in project_map.cpp.
//
// Resume. The token is the sha of a commit. plan() lists the commits the
// token's commit can reach (walked by earlier runs) and those it cannot (to
// walk now), in git's topological order. The first list is replayed through
// the Walk without reading a file, which rebuilds what the earlier runs knew:
// which analyses exist, which file belongs to which, the blob each path was
// left with. The second list is then walked for real.
//
// The token of a batch is the last commit that has no pending analysis at or
// before it: an analysis whose record is in but whose collection is not yet
// complete holds the token before its record, so everything up to a token is
// written. At the end of the walk the analyses still pending are folded with
// what they have, and the token is the head.

#include "pychron/dvc/project_adapter.hpp"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <unordered_set>
#include <utility>

#include "project_import.hpp"
#include "pychron/ingest/ids.hpp"

namespace pychron::dvc {

namespace {

// Commits whose changes are listed in one git call while replaying.
constexpr std::size_t kReplayChunk = 2000;
// Analyses folded per batch at the end of the walk, so that a repository full
// of analyses that never became complete is not read in one piece.
constexpr std::size_t kTailCollects = 200;

constexpr std::string_view kRewritten = "history was rewritten";

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
    walk_ = detail::Walk{};
    mapper_.emplace(config_, url_, reader_, walk_, state);
    order_.clear();
    tags_.clear();
    tail_.clear();
    walked_ = finished_ = false;

    auto all = reader_.rev_list(std::nullopt);
    if (!all) return fail(all.error());
    if (resume_token && !resume_token->empty()) {
      auto rest = reader_.rev_list(*resume_token);
      if (!rest) {
        // GitReader says so itself when the token is not an ancestor of the
        // head; a token the repository does not have at all is the same case.
        std::string what = rest.error().what;
        if (what.find(kRewritten) == std::string::npos)
          what = "resume token " + *resume_token + " is not in the history of " + config_.git.branch + ": " +
                 std::string(kRewritten) + " (" + what + ")";
        return fail(rest.error().kind, std::move(what));
      }
      const std::unordered_set<std::string> ahead(rest->begin(), rest->end());
      for (const auto& sha : *all)
        if (!ahead.contains(sha)) order_.push_back(sha);
      first_ = order_.size();
      for (const auto& sha : *all)
        if (ahead.contains(sha)) order_.push_back(sha);
    } else {
      order_ = std::move(*all);
      first_ = 0;
    }

    planned_ = true;
    next_ = first_;
    if (first_ == order_.size()) return 0;  // nothing new: no batch will be asked for

    for (std::size_t begin = 0; begin < first_; begin += kReplayChunk)
      if (auto r = walk(begin, std::min(first_, begin + kReplayChunk), nullptr); !r) return fail(r.error());
    walk_.assume_written();

    auto tags = reader_.tags();
    if (!tags) return fail(tags.error());
    tags_ = std::move(*tags);
    return static_cast<int>(order_.size() - first_);
  }

  Result<std::optional<ingest::ImportBatch>> next_batch() {
    if (!planned_) return fail(ErrorKind::Config, "project adapter: next_batch() before plan()");
    if (finished_ || (!walked_ && next_ == order_.size())) return std::optional<ingest::ImportBatch>{};

    ingest::ImportBatch batch;
    batch.head = reader_.head();
    batch.total = static_cast<int>(order_.size());
    std::vector<detail::Work> work;

    if (!walked_) {
      // A batch ends at a tagged commit: its bookmark captures the heads as
      // they are once the batch is written.
      std::size_t end = std::min(order_.size(), next_ + static_cast<std::size_t>(config_.batch_commits));
      for (std::size_t i = next_; i < end; ++i)
        if (tagged(order_[i])) end = i + 1;
      const std::span<const std::string> slice(order_.data() + next_, end - next_);
      auto commits = reader_.commits(slice);
      if (!commits) return fail(commits.error());
      mapper_->remember(std::move(*commits));
      if (auto r = walk(next_, end, &work); !r) return fail(r.error());
      next_ = end;
      if (next_ == order_.size()) {
        walked_ = true;
        const auto pending = walk_.pending();
        tail_.assign(pending.begin(), pending.end());
      }
    }
    if (walked_) {
      for (std::size_t n = 0; n < kTailCollects && !tail_.empty(); ++n) {
        walk_.force(*tail_.front(), work);
        tail_.pop_front();
      }
      if (tail_.empty()) {
        walk_.orphans(work);
        finished_ = true;
      }
    }

    if (auto r = mapper_->map(work, batch); !r) return fail(r.error());

    batch.done = static_cast<int>(next_);
    // The bookmarks of the last commit wait for the last batch, which holds
    // the analyses folded at the end of the walk.
    if (finished_ || !walked_)
      for (const auto& tag : tags_)
        if (tag.commit == order_[next_ - 1])
          if (auto r = mapper_->bookmark(tag, batch); !r) return fail(r.error());

    if (finished_) {
      batch.resume_token = order_.back();
    } else if (!walked_) {
      const auto pending = walk_.earliest_pending();
      const std::size_t reached = pending ? static_cast<std::size_t>(*pending) : next_;  // one past the token
      if (reached > first_) batch.resume_token = order_[reached - 1];
    }
    return std::optional<ingest::ImportBatch>{std::move(batch)};
  }

 private:
  bool tagged(const std::string& sha) const {
    return std::any_of(tags_.begin(), tags_.end(), [&](const GitTag& tag) { return tag.commit == sha; });
  }

  // Applies commits [begin, end) of the walk order. `out` null: replay.
  Result<void> walk(std::size_t begin, std::size_t end, std::vector<detail::Work>* out) {
    const std::span<const std::string> slice(order_.data() + begin, end - begin);
    auto changes = reader_.changes(slice);
    if (!changes) return fail(changes.error());
    // Grouped by commit in the order asked; a commit that changes nothing is absent.
    std::size_t at = 0;
    for (std::size_t i = begin; i < end; ++i) {
      std::size_t stop = at;
      while (stop < changes->size() && (*changes)[stop].commit == order_[i]) ++stop;
      walk_.apply(static_cast<int>(i), std::span<const GitChange>(changes->data() + at, stop - at), out);
      at = stop;
    }
    if (at != changes->size())
      return fail(ErrorKind::Protocol, "git: changes of commit " + (*changes)[at].commit + " were not asked for");
    return {};
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
  bool walked_ = false;                 // every commit is applied
  std::deque<detail::Track*> tail_;     // analyses still pending when the walk ended
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
