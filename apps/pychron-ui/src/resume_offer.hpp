#pragma once

// The offer to resume a queue that stopped (queue-tables design 5): what the
// executor's state file says, set against the queue that is open now.

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include <QDateTime>
#include <QString>

#include "pychron/experiment/executor/executor.hpp"
#include "pychron/experiment/model/run_spec.hpp"
#include "pychron/experiment/run/state.hpp"

namespace pychron::ui {

struct ResumeOffer {
  std::size_t next_row = 0;
  // "The queue %1 stopped at row %2 (%3) at %4. Resume from row %2?"
  QString question;
  // "It was stopped." (state "finalizing" or "idle"), else
  // "It did not finish (the program ended while it ran)."
  QString how;
  // Empty when resumable, else "Row %1 is %2 now; the file has %3."
  QString mismatch;
  // The file's runs[] as statuses; the last entry for a row wins, and a state
  // the run does not know leaves its row out.
  std::vector<std::pair<std::size_t, experiment::run::RunState>> statuses;
  [[nodiscard]] bool resumable() const noexcept { return mismatch.isEmpty(); }
};

// nullopt unless file.queue == queue.name and 0 < file.next_row < queue.runs.size().
// %1 the queue's name, %2 next_row + 1 (the # column), %3 that row's
// identifier now, %4 `written` as "yyyy-MM-dd HH:mm". Each runs[] entry's
// identifier is compared with the queue's at its row; the first that differs
// (or whose row is past the queue's end) is the mismatch.
[[nodiscard]] std::optional<ResumeOffer> resume_offer(const experiment::executor::ExecutorStateFile& file,
                                                      const experiment::QueueSpec& queue, const QDateTime& written);

}  // namespace pychron::ui
