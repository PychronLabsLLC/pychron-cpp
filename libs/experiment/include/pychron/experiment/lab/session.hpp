#pragma once

// LabSession (experiment-window design 4.2): runs queues against a lab on
// live (or simulated) hardware. Shared by `elctl exp run` and the UI.
//
// The run services (script host, valve services, spectrometer and peak-center
// ports, instrument metrics, persisters, aliquots) are assembled once, in the
// constructor. start() runs Executor::execute on a session-owned thread on a
// private copy of the queue; events go to the line's bus as usual, plus
// QueueEnded when the executor returns. A free-running ScanService is paused
// for the queue (measurement needs the acquisition engine) and resumed after.
//
// Controls are thread-safe and non-blocking. The destructor aborts a running
// queue and joins it.
//
// The lab's notifications (Lab::notifications) go out from a session-owned
// Notifier: a failed run as it finishes, the queue's end after QueueEnded.
// Each delivery is published as NotificationSent.

#include <filesystem>
#include <functional>
#include <map>
#include <string_view>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/experiment/executor/executor.hpp"
#include "pychron/experiment/lab/lab.hpp"
#include "pychron/experiment/lab/notifier.hpp"

namespace pychron::systems {
class ExtractionLine;
}
namespace pychron::spectrometer {
class Spectrometer;
class ScanService;
}  // namespace pychron::spectrometer

namespace pychron::experiment::lab {

struct SessionHardware {
  systems::ExtractionLine& line;  // started; its clock, scheduler and bus are the session's
  spectrometer::Spectrometer* spectrometer = nullptr;
  spectrometer::ScanService* scan = nullptr;  // paused while a queue runs
  // Whether the extraction device `driver` is simulated (a simulated camera
  // may only centre holes on a simulated stage). Empty: asked of the line.
  std::function<bool(std::string_view driver)> simulated;
};

struct SessionOptions {
  std::filesystem::path data;            // records/, spool/, executor_state.json
  executor::ExecutorOptions executor;    // state_file defaults to <data>/executor_state.json
  ProcessRunner notify;                  // runs the notification programs; empty: run_process
};

// Published on the line's bus when a queue started by a session ends, after
// any paused scan has been resumed.
struct QueueEnded {
  executor::QueueResult result;
};

class LabSession {
 public:
  // `lab` and the hardware must outlive the session.
  LabSession(const Lab& lab, SessionHardware hardware, SessionOptions options);
  ~LabSession();
  LabSession(const LabSession&) = delete;
  LabSession& operator=(const LabSession&) = delete;

  // Config error if a queue is running or `queue` does not check against the
  // lab (the message names the first error and how many there are).
  Result<void> start(QueueSpec queue, std::size_t from_row = 0);

  void stop();
  void cancel();
  void abort();
  void truncate(bool quick = false);
  // Replaces the running queue's runs with `queue.runs` (Executor::edit:
  // rows the executor has reached must be unchanged, `base` is the version
  // the edit was made against). The whole queue must check against the lab.
  // Queue-level fields are not editable while running and are ignored.
  Result<std::uint64_t> edit(std::uint64_t base, const QueueSpec& queue);

  bool running() const;
  executor::ExecutorState state() const;
  // Joins the queue thread; the last queue's result, nullopt if none ran.
  // start() and wait() are called from one thread (the owner's).
  std::optional<executor::QueueResult> wait();
  std::size_t pending_saves() const;  // records still in the spool
  TimePoint now() const;              // the line's clock (simulated or not)
  // Sends a test message on every configured channel (non-blocking).
  void notify_test();
  Notifier& notifier() noexcept { return *notifier_; }

  // What the lab asks of an extraction device that this session cannot do:
  // a camera that cannot be used to centre holes on it (a simulated camera
  // over a real laser, frames that cannot be opened). Fixed for the session.
  // A queue that uses such a device is not started: the lab is put right
  // first, rather than run uncentred without anyone having said so.
  std::vector<std::string> problems() const;

  const Lab& lab() const noexcept { return lab_; }
  const std::filesystem::path& data() const noexcept { return options_.data; }
  bool has_spectrometer() const noexcept;
  static Result<std::size_t> resume_row(const std::filesystem::path& data);

 private:
  struct Services;

  void run(QueueSpec queue, std::size_t from_row);
  Result<void> check(const QueueSpec& queue) const;  // against the lab; names the first error

  const Lab& lab_;
  SessionHardware hardware_;
  SessionOptions options_;
  std::map<std::string, std::string, std::less<>> device_problems_;  // by extraction device
  std::unique_ptr<Services> services_;
  std::unique_ptr<Notifier> notifier_;

  mutable std::mutex mutex_;
  // The current or last queue's. Controls call it outside mutex_, so a bus
  // subscriber may call back into the session.
  std::shared_ptr<executor::Executor> executor_;
  std::shared_ptr<executor::Executor> active() const;  // executor_ while running
  std::optional<executor::QueueResult> result_;
  bool running_ = false;
  std::thread thread_;
};

}  // namespace pychron::experiment::lab
