#pragma once

// A source adapter that serves scripted batches.

#include <cstddef>
#include <optional>
#include <string>
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
    return std::optional<ImportBatch>{batches_[next_++]};
  }

 private:
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
