#pragma once

// Persister boundary (experiment spec 8.4, ADR-0002).
//
//   IAnalysisPersister  where finished runs go (FilePersister here; the
//                       database persister and git mirror come later)
//   Spool               local, durable copy written before anything else
//   SavePipeline        spool first, then hand the record to the persister;
//                       a persister failure never blocks the next run, is
//                       told to the caller (Saved::pending) and is retried by
//                       flush() and on startup (recover())
//   SpoolFlusher        flushes the pipeline after every run and, while
//                       anything is pending, on a timer (a scheduler job; no
//                       run waits on it); publishes SpoolStatus
//   AliquotAllocator    one source of truth for aliquot numbers
//
// Record files are deterministic JSON (record/serialize.hpp).

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/core/process_log.hpp"
#include "pychron/core/scheduler.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/experiment/model/run_spec.hpp"
#include "pychron/experiment/record/types.hpp"

namespace pychron::experiment::persist {

// An identifier's last run as a persister knows it.
struct LastRun {
  int aliquot = 0;
  int step = -1;  // step_index of its step; -1: no step
  bool operator==(const LastRun&) const = default;
};

class IAnalysisPersister {
 public:
  virtual ~IAnalysisPersister() = default;
  // Next free aliquot for an identifier (1 for a new identifier).
  virtual Result<int> next_aliquot(const std::string& identifier) = 0;
  virtual Result<void> begin_run(const RunIdentity& id, const QueueSpec& queue) = 0;
  virtual Result<void> save_extraction(const record::AnalysisRecord& record) = 0;
  virtual Result<void> save_analysis(const record::AnalysisRecord& record) = 0;
  virtual Result<void> save_artifact(const std::string& uuid, const std::string& name,
                                     const std::vector<std::uint8_t>& bytes) = 0;
  virtual Result<void> flush() = 0;
  // What would stop this queue's runs from being saved, one line each; empty
  // when nothing would. Asked of the queue as it will run (rows_from): a
  // skipped row is not checked as a run.
  virtual Result<std::vector<std::string>> check_queue(const QueueSpec& queue) = 0;
  // The identifier's run with the highest aliquot, and within it the highest
  // step, among runs saved or waiting to be sent. nullopt: none, or this
  // persister cannot say (the default).
  virtual Result<std::optional<LastRun>> last_run(const std::string& identifier) {
    (void)identifier;
    return std::optional<LastRun>{};
  }
};

// `queue` as a start at `from_row` will run it, for check_queue: the rows
// before `from_row` marked skipped, since they are not run (again). A row an
// edit may no longer change is such a row too.
QueueSpec rows_from(QueueSpec queue, std::size_t from_row);

// Records directory:
//   <root>/<identifier>/<identifier>-<aliquot><step>.json     analyses
//   <root>/<identifier>/<identifier>-<aliquot><step>.extraction.json
//   <root>/artifacts/<uuid>/<name>
// next_aliquot() is one more than the highest aliquot on disk or handed out
// this session for that identifier.
class FilePersister final : public IAnalysisPersister {
 public:
  explicit FilePersister(std::filesystem::path root) : root_(std::move(root)) {}

  Result<int> next_aliquot(const std::string& identifier) override;
  Result<void> begin_run(const RunIdentity& id, const QueueSpec& queue) override;
  Result<void> save_extraction(const record::AnalysisRecord& record) override;
  Result<void> save_analysis(const record::AnalysisRecord& record) override;
  Result<void> save_artifact(const std::string& uuid, const std::string& name,
                             const std::vector<std::uint8_t>& bytes) override;
  Result<void> flush() override { return {}; }
  Result<std::vector<std::string>> check_queue(const QueueSpec&) override { return std::vector<std::string>{}; }
  // From the names under <root>/<identifier>/ (analysis and extraction
  // files). Reads the disk only: hands out no aliquot.
  Result<std::optional<LastRun>> last_run(const std::string& identifier) override;

  std::filesystem::path analysis_path(const record::AnalysisRecord& record) const;

 private:
  int highest_on_disk(const std::string& identifier) const;

  std::filesystem::path root_;
  std::mutex mutex_;
  std::map<std::string, int> issued_;  // identifier -> highest aliquot handed out
  Logger log_ = process_logger("store");  // the records folder is this process's store
};

// Pending records, one JSON file per record uuid, written atomically.
class Spool {
 public:
  explicit Spool(std::filesystem::path dir) : dir_(std::move(dir)) {}

  Result<void> put(const record::AnalysisRecord& record);
  Result<void> remove(const std::string& uuid);
  // Uuids of pending records, oldest first.
  Result<std::vector<std::string>> pending() const;
  Result<record::AnalysisRecord> get(const std::string& uuid) const;
  // Whether the record is still pending (false once it has been sent).
  bool contains(const std::string& uuid) const;
  const std::filesystem::path& dir() const noexcept { return dir_; }

 private:
  std::filesystem::path dir_;
  Logger log_ = process_logger("spool");
};

// What became of a record that was spooled.
struct Saved {
  // Why the persister did not take it: the record is in the spool and will be
  // sent again (flush(), recover()). Empty when the persister has it, and
  // when the hand-off was posted and its outcome is not known yet.
  std::optional<Error> pending;
};

// Spool-first save. With a `post` function the hand-off to the persister runs
// through it (e.g. onto a Scheduler worker); without one it runs inline.
class SavePipeline {
 public:
  using Post = std::function<void(std::function<void()>)>;

  SavePipeline(Spool& spool, IAnalysisPersister& persister, Post post = {})
      : spool_(spool), persister_(persister), post_(std::move(post)) {}

  // Fails only if the record cannot be spooled (then it is lost: Failed(save_error)).
  // A persister failure leaves it pending, and the answer says why.
  Result<Saved> save(const record::AnalysisRecord& record);

  // Retries every pending record; returns how many were persisted. The first
  // failure is returned only if nothing could be persisted.
  Result<std::size_t> flush();
  // Startup: re-sends records left in the spool by an earlier session.
  Result<std::size_t> recover() { return flush(); }

  std::size_t pending() const;
  // "<record uuid>: <why>" for the hand-off that failed last, until that
  // record has been sent; empty when none failed or the last one has gone.
  std::optional<std::string> last_error() const;

 private:
  // True when this call sent the record; false when it had already left the
  // spool (sent by a flush or a save on another thread meanwhile).
  Result<bool> hand_off(const std::string& uuid);

  Logger log_ = process_logger("spool");
  Spool& spool_;
  IAnalysisPersister& persister_;
  Post post_;
  mutable std::mutex mutex_;
  std::optional<std::pair<std::string, std::string>> last_error_;  // record uuid, why
};

// What the spool holds (executor hardening spec 4.8). Published on the bus
// whenever it changes, by the SpoolFlusher.
struct SpoolStatus {
  std::size_t pending = 0;
  // "<record uuid>: <why>" for the last record that could not be sent (or why
  // the flush failed as a whole); nothing once it has gone.
  std::optional<std::string> last_error;
  bool operator==(const SpoolStatus&) const = default;
};

// Flushes a SavePipeline off the control path: once after every run
// (flush_now()), then, while anything is pending, every `interval`. While the
// flushes keep failing the wait doubles up to `max_interval`; a flush that
// succeeds brings it back to `interval`. With nothing pending no job is left
// scheduled. Every flush is a one-shot job on the scheduler, never on the
// caller's thread, and no two run at once.
//
// Borrowed: pipeline, scheduler, clock and bus outlive the flusher. The
// destructor cancels the job waiting and waits (in the clock) for one that is
// running.
class SpoolFlusher {
 public:
  struct Options {
    pychron::Duration interval = std::chrono::minutes(5);
    pychron::Duration max_interval = std::chrono::minutes(30);
  };

  SpoolFlusher(SavePipeline& pipeline, Scheduler& scheduler, const Clock& clock, SignalBus* bus)
      : SpoolFlusher(pipeline, scheduler, clock, bus, Options{}) {}
  SpoolFlusher(SavePipeline& pipeline, Scheduler& scheduler, const Clock& clock, SignalBus* bus, Options options);
  ~SpoolFlusher();
  SpoolFlusher(const SpoolFlusher&) = delete;
  SpoolFlusher& operator=(const SpoolFlusher&) = delete;
  SpoolFlusher(SpoolFlusher&&) = delete;
  SpoolFlusher& operator=(SpoolFlusher&&) = delete;

  // A flush as soon as the scheduler can run it (after a run, at the start of
  // a queue). Returns at once.
  void flush_now();
  // The status last published.
  SpoolStatus status() const;

 private:
  void run(std::uint64_t seq);
  void schedule_locked(pychron::Duration delay);  // caller holds mutex_

  SavePipeline& pipeline_;
  Scheduler& scheduler_;
  Logger log_ = process_logger("spool");
  const Clock& clock_;
  SignalBus* bus_;
  Options options_;

  mutable std::mutex mutex_;  // the fields below; never held across a flush
  std::condition_variable idle_;  // running_ went false; waited on through clock_
  // Jobs scheduled whose run() has not yet taken mutex_, by sequence number.
  std::map<std::uint64_t, JobId> jobs_;
  std::uint64_t next_seq_ = 0;
  pychron::Duration delay_;                // wait before the next timed flush
  bool running_ = false;          // a flush is in progress
  bool again_ = false;            // flush_now() while one was running
  bool closing_ = false;
  SpoolStatus status_;
};

struct Allocation {
  int aliquot = 0;
  std::string step;
};

// User-fixed aliquots are validated (>= 1) and used as given. Otherwise a
// step that comes after the last step this allocator gave its identifier
// (B or C after A) takes that step's aliquot, so the steps of a step heat are
// one aliquot; a run with no step, step "A", a step not after the last one
// (a new heating whose A was skipped) or an identifier nothing was given for
// in this session takes the persister's next_aliquot(). A run with no step
// leaves what is remembered alone. Steps are kept as given.
//
// What is remembered lives as long as the allocator. The first time a step
// after A comes for an identifier nothing is remembered for, the persister's
// last_run() stands in for that memory: when the identifier's last saved run
// has a step before this one, its aliquot is taken and remembered, so a
// heating resumed after a restart, in another queue or on another day keeps
// its aliquot. It is asked at most once per identifier; an error from it
// fails the allocation.
class AliquotAllocator {
 public:
  explicit AliquotAllocator(IAnalysisPersister& persister) : persister_(persister) {}
  Result<Allocation> allocate(const RunIdentity& id);

 private:
  struct Heating {
    int aliquot = 0;
    int step = -1;  // step_index of the last step given
  };
  IAnalysisPersister& persister_;
  std::mutex mutex_;                     // guards last_ and asked_, never held across the persister
  std::map<std::string, Heating> last_;  // identifier -> its last stepped allocation
  std::set<std::string> asked_;          // identifiers last_run() was asked about
};

}  // namespace pychron::experiment::persist
