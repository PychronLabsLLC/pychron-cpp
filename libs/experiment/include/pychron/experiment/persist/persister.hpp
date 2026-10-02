#pragma once

// Persister boundary (experiment spec 8.4, ADR-0002).
//
//   IAnalysisPersister  where finished runs go (FilePersister here; the
//                       database persister and git mirror come later)
//   Spool               local, durable copy written before anything else
//   SavePipeline        spool first, then hand the record to the persister;
//                       a persister failure never blocks the next run and is
//                       retried by flush() and on startup (recover())
//   AliquotAllocator    one source of truth for aliquot numbers
//
// Record files are deterministic JSON (record/serialize.hpp).

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/experiment/model/run_spec.hpp"
#include "pychron/experiment/record/types.hpp"

namespace pychron::experiment::persist {

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
};

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

  std::filesystem::path analysis_path(const record::AnalysisRecord& record) const;

 private:
  int highest_on_disk(const std::string& identifier) const;

  std::filesystem::path root_;
  std::mutex mutex_;
  std::map<std::string, int> issued_;  // identifier -> highest aliquot handed out
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
  const std::filesystem::path& dir() const noexcept { return dir_; }

 private:
  std::filesystem::path dir_;
};

// Spool-first save. With a `post` function the hand-off to the persister runs
// through it (e.g. onto a Scheduler worker); without one it runs inline.
class SavePipeline {
 public:
  using Post = std::function<void(std::function<void()>)>;

  SavePipeline(Spool& spool, IAnalysisPersister& persister, Post post = {})
      : spool_(spool), persister_(persister), post_(std::move(post)) {}

  // Fails only if the record cannot be spooled (then it is lost: Failed(save_error)).
  // A persister failure leaves it pending.
  Result<void> save(const record::AnalysisRecord& record);

  // Retries every pending record; returns how many were persisted. The first
  // failure is returned only if nothing could be persisted.
  Result<std::size_t> flush();
  // Startup: re-sends records left in the spool by an earlier session.
  Result<std::size_t> recover() { return flush(); }

  std::size_t pending() const;
  std::optional<std::string> last_error() const;

 private:
  Result<void> hand_off(const std::string& uuid);

  Spool& spool_;
  IAnalysisPersister& persister_;
  Post post_;
  mutable std::mutex mutex_;
  std::optional<std::string> last_error_;
};

struct Allocation {
  int aliquot = 0;
  std::string step;
};

// User-fixed aliquots are validated (>= 1) and used as given; otherwise the
// persister's next_aliquot(). Steps are kept as given.
class AliquotAllocator {
 public:
  explicit AliquotAllocator(IAnalysisPersister& persister) : persister_(persister) {}
  Result<Allocation> allocate(const RunIdentity& id);

 private:
  IAnalysisPersister& persister_;
};

}  // namespace pychron::experiment::persist
