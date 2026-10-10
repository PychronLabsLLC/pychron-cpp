#pragma once

// Where an acquisition program's finished runs go: the DVC store. Records
// reach it through persist::SavePipeline, which spools each one first, so a
// store that is away, or an identifier the catalog does not have yet, leaves
// the record in the spool to be sent later (SavePipeline::recover()).
//
// The persister only reads the catalog (an identifier, a mass spectrometer, an
// extract device) and ingests; it never creates a catalog row.

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/experiment/persist/persister.hpp"
#include "pychron/persistence/store.hpp"

namespace pychron::experiment::store {

// `store` must outlive the persister. Used from one thread at a time (the
// queue's); the lock inside guards the two maps and is never held across a
// store call.
class StorePersister final : public persist::IAnalysisPersister {
 public:
  // `data` is the session's data folder: the spool is <data>/spool, partial
  // records and artifacts go under <data>/records.
  StorePersister(persistence::IStore& store, persistence::Uuid client, std::filesystem::path data);

  // 1 + the highest of: the store's last aliquot of the identifier, the
  // spool's, and those this persister has handed out. When the store cannot
  // be read the last value read for the identifier stands in; with none, the
  // error is returned.
  Result<int> next_aliquot(const std::string& identifier) override;
  Result<void> begin_run(const RunIdentity& id, const QueueSpec& queue) override;
  Result<void> save_extraction(const record::AnalysisRecord& record) override;
  // Ingests the record's raw series, then the analysis. An item the store has
  // already ingested is a success. An identifier, mass spectrometer or
  // extract device the catalog lacks is refused with a text that says what to
  // add (the error's kind and code are the store's).
  Result<void> save_analysis(const record::AnalysisRecord& record) override;
  Result<void> save_artifact(const std::string& uuid, const std::string& name,
                             const std::vector<std::uint8_t>& bytes) override;
  Result<void> flush() override { return {}; }
  Result<std::vector<std::string>> check_queue(const QueueSpec& queue) override;

 private:
  persistence::IStore& store_;
  persistence::Uuid client_;
  std::filesystem::path data_;
  persist::FilePersister files_;
  std::mutex mutex_;
  std::map<std::string, int> read_;    // identifier -> last aliquot read from the store
  std::map<std::string, int> issued_;  // identifier -> highest aliquot handed out
};

// The store of an acquisition program and its persister. Opens with
// migrate = false and registers the acquisition client.
struct Acquisition {
  std::unique_ptr<persistence::IStore> store;
  std::unique_ptr<StorePersister> persister;  // refers to *store
};

// A schema that is behind is refused with the command that brings it up to
// date, naming the database (without its password).
Result<Acquisition> open_acquisition(const std::string& url, std::filesystem::path data,
                                     const std::string& software_version);

// `url` as it may be shown: the password of a postgresql://user:password@host
// url removed. Any other url is unchanged.
std::string without_password(const std::string& url);

}  // namespace pychron::experiment::store
