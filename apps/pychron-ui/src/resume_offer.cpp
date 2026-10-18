#include "resume_offer.hpp"

#include <algorithm>
#include <map>

#include <QCoreApplication>

namespace pychron::ui {

namespace {

QString tr(const char* text) { return QCoreApplication::translate("ResumeOffer", text); }

}  // namespace

std::optional<ResumeOffer> resume_offer(const experiment::executor::ExecutorStateFile& file,
                                        const experiment::QueueSpec& queue, const QDateTime& written) {
  if (file.queue != queue.name || file.next_row == 0 || file.next_row >= queue.runs.size()) return std::nullopt;

  ResumeOffer offer;
  offer.next_row = file.next_row;
  const QString name = QString::fromStdString(queue.name);
  const QString identifier = QString::fromStdString(queue.runs[file.next_row].id.identifier);
  offer.question = tr("The queue %1 stopped at row %2 (%3) at %4. Resume from row %2?")
                       .arg(name, QString::number(file.next_row + 1), identifier,
                            written.toString(QStringLiteral("yyyy-MM-dd HH:mm")));
  // Every end, clean or stopped, writes "finalizing" last; anything else is
  // a queue the program left while it ran.
  const bool stopped = file.state == "finalizing" || file.state == "idle";
  offer.how = stopped ? tr("It was stopped.") : tr("It did not finish (the program ended while it ran).");

  std::map<std::size_t, experiment::run::RunState> statuses;
  for (const auto& run : file.runs) {
    if (offer.mismatch.isEmpty()) {
      const bool known = run.row < queue.runs.size();
      if (!known || queue.runs[run.row].id.identifier != run.identifier) {
        const QString now = known ? QString::fromStdString(queue.runs[run.row].id.identifier) : tr("not there");
        offer.mismatch = tr("Row %1 is %2 now; the file has %3.")
                             .arg(QString::number(run.row + 1), now, QString::fromStdString(run.identifier));
      }
    }
    if (const auto state = experiment::run::run_state_from(run.state)) statuses[run.row] = *state;
    else statuses.erase(run.row);  // the last entry wins, even one that says nothing
  }
  offer.statuses.assign(statuses.begin(), statuses.end());
  return offer;
}

}  // namespace pychron::ui
