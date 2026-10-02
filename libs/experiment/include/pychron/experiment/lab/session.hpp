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

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "pychron/core/error.hpp"
#include "pychron/experiment/executor/executor.hpp"
#include "pychron/experiment/lab/lab.hpp"

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
};

struct SessionOptions {
  std::filesystem::path data;            // records/, spool/, executor_state.json
  executor::ExecutorOptions executor;    // state_file defaults to <data>/executor_state.json
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

  bool running() const;
  executor::ExecutorState state() const;
  // Joins the queue thread; the last queue's result, nullopt if none ran.
  // start() and wait() are called from one thread (the owner's).
  std::optional<executor::QueueResult> wait();
  std::size_t pending_saves() const;  // records still in the spool

  const Lab& lab() const noexcept { return lab_; }
  const std::filesystem::path& data() const noexcept { return options_.data; }
  bool has_spectrometer() const noexcept;
  static Result<std::size_t> resume_row(const std::filesystem::path& data);

 private:
  struct Services;

  void run(QueueSpec queue, std::size_t from_row);

  const Lab& lab_;
  SessionHardware hardware_;
  SessionOptions options_;
  std::unique_ptr<Services> services_;

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
