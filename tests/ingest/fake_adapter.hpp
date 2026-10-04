#pragma once

// A source adapter that serves scripted batches, and scripted units for the
// verifier.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pychron/ingest/adapter.hpp"

namespace pychron::ingest::testing {

class FakeAdapter final : public ISourceAdapter {
 public:
  FakeAdapter(SourceDescription description, std::vector<ImportBatch> batches)
      : description_(std::move(description)), batches_(std::move(batches)) {}

  // next_batch() fails instead of serving batch `n` (1-based); nullopt: never.
  void fail_at(std::optional<std::size_t> n) { fail_at_ = n; }
  // Skip the batches up to and including the one whose resume_token was
  // passed to plan(), as a real adapter does. Off: plan() rewinds to batch 1.
  void honour_token(bool on) { honour_token_ = on; }

  // The source's walk order, for a script that does not show every commit or
  // shows them out of order. Without it the order is that of the commits as
  // the batches name them: per batch the analyses (record, then root files),
  // then the changesets.
  void walk(std::vector<std::string> commits) { walk_ = std::move(commits); }

  // What for_each_unit() hands to its visitor.
  void units(std::vector<SourceUnit> scripted) { units_ = std::move(scripted); }

  const std::optional<std::string>& planned_token() const { return planned_token_; }
  int plans() const { return plans_; }
  std::size_t served() const { return served_; }

  Result<SourceDescription> describe() override { return description_; }

  Result<int> plan(std::optional<std::string> resume_token, IImportState&) override {
    ++plans_;
    planned_token_ = std::move(resume_token);
    next_ = 0;
    if (honour_token_ && planned_token_)
      for (std::size_t i = 0; i < batches_.size(); ++i)
        if (batches_[i].resume_token == *planned_token_) next_ = i + 1;
    return static_cast<int>(batches_.size() - next_);
  }

  Result<std::optional<ImportBatch>> next_batch() override {
    if (fail_at_ && next_ + 1 == *fail_at_) return fail(ErrorKind::Io, "git: scripted failure");
    if (next_ >= batches_.size()) return std::optional<ImportBatch>{};
    ++served_;
    // A revision the script gave no place takes its commit's.
    ImportBatch batch = batches_[next_++];
    for (auto& changeset : batch.changesets)
      for (auto& revision : changeset.revisions)
        if (!revision.order) revision.order = place(revision.key.commit);
    return std::optional<ImportBatch>{std::move(batch)};
  }

  Result<std::optional<std::int64_t>> order_of(std::string_view commit) override { return place(commit); }

  Result<void> for_each_unit(IImportState&, const std::function<Result<void>(const SourceUnit&)>& visit) override {
    for (const auto& unit : units_)
      if (auto r = visit(unit); !r) return r;
    return {};
  }

 private:
  std::optional<std::int64_t> place(std::string_view commit) const {
    std::vector<std::string> seen = walk_;
    const auto note = [&](const std::string& sha) {
      if (!sha.empty() && std::find(seen.begin(), seen.end(), sha) == seen.end()) seen.push_back(sha);
    };
    if (seen.empty())
      for (const auto& batch : batches_) {
        for (const auto& analysis : batch.analyses)
          for (const SourceKey* key : {&analysis.keys.record, &analysis.keys.signals, &analysis.keys.intercepts,
                                       &analysis.keys.baselines, &analysis.keys.blanks, &analysis.keys.icfactors,
                                       &analysis.keys.tags})
            note(key->commit);
        for (const auto& changeset : batch.changesets) note(changeset.commit);
      }
    const auto found = std::find(seen.begin(), seen.end(), commit);
    if (found == seen.end()) return std::nullopt;
    return static_cast<std::int64_t>(found - seen.begin());
  }

  std::vector<std::string> walk_;
  std::vector<SourceUnit> units_;
  SourceDescription description_;
  std::vector<ImportBatch> batches_;
  std::optional<std::size_t> fail_at_;
  bool honour_token_ = false;
  std::optional<std::string> planned_token_;
  std::size_t next_ = 0;
  std::size_t served_ = 0;
  int plans_ = 0;
};

}  // namespace pychron::ingest::testing
