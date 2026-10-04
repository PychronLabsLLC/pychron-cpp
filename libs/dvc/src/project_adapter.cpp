// ProjectRepoAdapter: the commit list, the batches cut from it, the resume
// token. What a commit means is decided in project_walk.cpp, what a file
// holds in project_map.cpp. The walk order, the changes of a merge and the
// token are those of commit_walk.hpp.
//
// Batches. A batch is batch_commits commits of the walk order. It ends
// earlier after a tagged commit (the bookmark captures the heads as they are
// once the batch is written) and after a commit that rewrites the record of
// an analysis already folded (the rewrite can change a run identity, and the
// writer stores a batch's analyses before its changesets: a later analysis
// that takes the freed run id must come in a later batch). Where the cuts
// fall does not change what is stored.
//
// Resume. plan() computes the order again and asks where the token leaves it
// (resume_point). The commits before that are first replayed through the Walk
// without reading a file, which rebuilds what the earlier run knew: which
// file belongs to which analysis, the blob each path was left with, which
// collections were folded. An analysis still pending at the token stays
// pending and is folded by the resumed walk, as an uninterrupted walk would.
//
// At the end of the walk the analyses still pending are folded with what they
// have. A later run that resumes from an "+end" token treats them as folded.

#include "pychron/dvc/project_adapter.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

#include "commit_walk.hpp"
#include "project_import.hpp"
#include "pychron/ingest/ids.hpp"

namespace pychron::dvc {

namespace {

// Commits whose changes are listed in one git call while replaying.
constexpr std::size_t kReplayChunk = 2000;

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
    place_.clear();
    tags_.clear();
    finished_ = planned_ = false;
    first_ = next_ = 0;

    auto all = reader_.rev_list(std::nullopt);
    if (!all) return fail(all.error());
    order_ = std::move(*all);
    place_ = detail::places(order_);

    const auto resume = detail::resume_point(order_, resume_token, reader_, config_.git, "project adapter");
    if (!resume) return fail(resume.error());
    first_ = resume->first;
    const bool resumed_at_end = resume->at_end;

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

  Result<void> check_token(const std::string& resume_token) const {
    auto all = reader_.rev_list(std::nullopt);
    if (!all) return fail(all.error());
    const auto resume = detail::resume_point(*all, resume_token, reader_, config_.git, "project adapter");
    if (!resume) return fail(resume.error());
    return {};
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
    batch.resume_token = detail::format_token(order_, next_ - 1, finished_);
    return std::optional<ingest::ImportBatch>{std::move(batch)};
  }

  // The walk an import makes from the first commit, with a ledger: every
  // file is settled in the batch that maps it, by the rows that batch would
  // leave for it.
  Result<void> for_each_unit(ingest::IImportState& state,
                             const std::function<Result<void>(const ingest::SourceUnit&)>& visit) {
    if (auto planned = plan(std::nullopt, state); !planned) return fail(planned.error());
    detail::Ledger ledger;
    detail::UnitAccount account(visit);
    walk_.observe(&ledger);
    mapper_->observe(&ledger);
    Result<void> done;
    for (;;) {
      auto batch = next_batch();
      if (!batch) {
        done = fail(batch.error());
        break;
      }
      if (!*batch) {
        done = account.finish();
        break;
      }
      done = account.settle(ledger, **batch);
      if (!done) break;
    }
    walk_.observe(nullptr);
    mapper_->observe(nullptr);
    planned_ = false;  // the walk is used up: an import plans again
    return done;
  }

  std::optional<std::int64_t> order_of(std::string_view commit) const { return detail::place_of(place_, commit); }

 private:
  bool tagged(const std::string& sha) const {
    return std::any_of(tags_.begin(), tags_.end(), [&](const GitTag& tag) { return tag.commit == sha; });
  }

  // Applies commits [begin, end) of the walk order and returns one past the
  // last commit applied: `end`, or earlier where a batch must end (Walk::apply).
  // `out` null: a replay, which applies them all. The Walk skips every path
  // that already has the blob a commit gives it, so of a merge what is left
  // are the files the merge moved away from the content last imported.
  Result<std::size_t> walk(std::size_t begin, std::size_t end, std::vector<detail::Work>* out) {
    std::vector<GitCommit> commits;
    auto applied = detail::walk_commits(reader_, order_, begin, end, commits,
                                        [&](std::size_t index, std::span<const GitChange> changes) {
                                          return walk_.apply(static_cast<int>(index), changes, out) && out != nullptr;
                                        });
    if (!applied) return fail(applied.error());
    if (out) mapper_->remember(std::move(commits));
    return *applied;
  }

  ProjectAdapterConfig config_;
  GitReader reader_;
  std::string url_;  // normalized

  bool planned_ = false;
  std::vector<std::string> order_;  // commits earlier runs walked, then those to walk
  detail::Places place_;            // of each commit in order_
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

Result<void> ProjectRepoAdapter::check_token(const std::string& resume_token) { return impl_->check_token(resume_token); }

Result<std::optional<ingest::ImportBatch>> ProjectRepoAdapter::next_batch() { return impl_->next_batch(); }

Result<void> ProjectRepoAdapter::for_each_unit(ingest::IImportState& state,
                                               const std::function<Result<void>(const ingest::SourceUnit&)>& visit) {
  return impl_->for_each_unit(state, visit);
}

Result<std::optional<std::int64_t>> ProjectRepoAdapter::order_of(std::string_view commit) {
  return impl_->order_of(commit);
}

}  // namespace pychron::dvc
