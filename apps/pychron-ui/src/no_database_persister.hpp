#pragma once

// The persister of a session whose database could not be opened. Every queue
// is refused at its start with the reason (check_queue), and nothing is saved
// through it: a record that did reach it stays in the spool.

#include <cstdint>
#include <string>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/experiment/persist/persister.hpp"

namespace pychron::ui {

class NoDatabasePersister final : public experiment::persist::IAnalysisPersister {
 public:
  // `reason`: why the database cannot be used, as the store said it.
  explicit NoDatabasePersister(const std::string& reason);

  // "the database cannot be reached: <reason>": check_queue's one line, and
  // the text of every error below.
  const std::string& why() const noexcept { return why_; }

  // Each an Io error with why().
  Result<int> next_aliquot(const std::string& identifier) override;
  Result<void> begin_run(const experiment::RunIdentity& id, const experiment::QueueSpec& queue) override;
  Result<void> save_extraction(const experiment::record::AnalysisRecord& record) override;
  Result<void> save_analysis(const experiment::record::AnalysisRecord& record) override;
  Result<void> save_artifact(const std::string& uuid, const std::string& name,
                             const std::vector<std::uint8_t>& bytes) override;
  Result<void> flush() override { return {}; }
  Result<std::vector<std::string>> check_queue(const experiment::QueueSpec& queue) override;

 private:
  std::string why_;
};

}  // namespace pychron::ui
