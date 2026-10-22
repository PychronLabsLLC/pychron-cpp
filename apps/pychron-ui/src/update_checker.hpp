#pragma once

// Telling the operator that a newer release of pychron-cpp exists (update
// check spec). One check at a time asks GitHub for the latest release on a
// thread of its own, so a slow network never stops the window from painting;
// the answer comes back on the GUI thread, where it is remembered in QSettings
// and said as a signal. Nothing here opens a dialog: what is shown, and when,
// is the window's.
//
// The worker waits on the outside world (a real socket, 10 s timeout) and is
// not a clock participant (AGENTS.md, "Time"). It touches no QSettings and
// emits nothing: it only posts its answer back to this object. The destructor
// joins it, so an owner that goes away with a check out waits for that check
// (at most the request's timeout) and the answer is never delivered.

#include <chrono>
#include <functional>
#include <memory>
#include <thread>

#include <QObject>
#include <QString>

#include "pychron/core/error.hpp"
#include "pychron/core/http.hpp"
#include "pychron/update/update.hpp"

class QSettings;

namespace pychron::ui {

// The check's policy in QSettings: `updates/check_on_startup` (bool, default
// true), `updates/last_check` (ISO 8601 UTC, to the second) and
// `updates/skipped_version` ("x.y.z"). A value that does not read is none.
[[nodiscard]] update::CheckPolicy load_update_policy(const QSettings& settings);
void save_update_policy(QSettings& settings, const update::CheckPolicy& policy);

class UpdateChecker : public QObject {
  Q_OBJECT

 public:
  // Called for each check; null in a build without a network client.
  using ClientFactory = std::function<std::unique_ptr<http::IHttpClient>()>;
  using Now = std::function<std::chrono::system_clock::time_point()>;

  // `settings` outlives the checker. `now` is the line's wall clock.
  UpdateChecker(QSettings& settings, update::Version running, Now now, ClientFactory make_client,
                QObject* parent = nullptr);
  ~UpdateChecker() override;
  UpdateChecker(const UpdateChecker&) = delete;
  UpdateChecker& operator=(const UpdateChecker&) = delete;
  UpdateChecker(UpdateChecker&&) = delete;
  UpdateChecker& operator=(UpdateChecker&&) = delete;

  // Where a failed automatic check is written, as a log line
  // ("INFO [update] ..." for the network, "WARN [update] ..." for an answer
  // that does not read). Unset, it is not written.
  void set_log(std::function<void(const QString& line)> log);

  // The automatic check: does nothing unless one is due (update::due) and
  // the build has a client.
  void check_on_startup();
  // Help > Check for Updates: whether or not one is due. Without a client,
  // `failed` at once.
  void check_now();
  // A check is out. Another asked for meanwhile is not made.
  [[nodiscard]] bool busy() const { return busy_; }
  // The version this program is.
  [[nodiscard]] update::Version running() const { return running_; }

  // The operator does not want to hear of `version` again.
  void skip(update::Version version);

 signals:
  // Automatic: only a release worth showing (newer, not skipped). Manual:
  // any release newer than the running one.
  void available(const pychron::update::Release& release, bool manual);
  // Manual only: nothing newer; `no_release` when the repository has none.
  void up_to_date(bool no_release);
  // `reason` is said to the operator: "Could not reach GitHub (...)" for the
  // network, "GitHub answered <status>", "Unexpected answer from GitHub (...)"
  // for an answer that does not read.
  void failed(const QString& reason, bool manual);
  // busy() changed: a check went out, or its answer came back.
  void busy_changed(bool busy);

 private:
  void start(bool manual, std::unique_ptr<http::IHttpClient> client);
  void finish(bool manual, const Result<update::Fetched>& result);
  void set_busy(bool busy);

  QSettings& settings_;
  update::Version running_;
  Now now_;
  ClientFactory make_client_;
  std::function<void(const QString&)> log_;
  bool busy_ = false;
  std::thread worker_;  // joined in the destructor's body, before anything else goes
};

}  // namespace pychron::ui
