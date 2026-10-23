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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/core/process_log.hpp"
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
  // The later, by (aliquot, step), of the store's last analysis of the
  // identifier and the spool's records of it; nullopt with neither. When the
  // store cannot be read the last answer read for the identifier stands in;
  // with none, the error is returned. Hands out nothing: next_aliquot() is not
  // moved by it.
  Result<std::optional<persist::LastRun>> last_run(const std::string& identifier) override;
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
  Logger log_ = process_logger("store");
  persistence::IStore& store_;
  persistence::Uuid client_;
  std::filesystem::path data_;
  persist::FilePersister files_;
  std::mutex mutex_;
  // identifier -> last run read from the store (aliquot 0: the store had none)
  std::map<std::string, persist::LastRun> read_;
  std::map<std::string, int> issued_;  // identifier -> highest aliquot handed out
};

struct AcquisitionImpl;

// A store opened for an acquisition program, and its persister. Every call on
// persister() runs on one thread owned by this object, the one that opened
// the store (a store handle works only on its own thread); the caller waits
// for it. Calls may come from any thread, one at a time or several; they are
// served one at a time, in the order they arrived.
//
// The worker is a plain thread on a std::condition_variable, deliberately not
// a Clock::Participant and holding no clock: to the simulated clock a caller
// blocked on the worker looks busy, so simulated time stands still for the
// length of the call, exactly as for a file write, and the worker itself never
// waits for simulated time.
//
// Destroying it finishes the calls already queued, closes the store on the
// worker and joins. No call may be in progress or made once destruction begins.
class Acquisition {
 public:
  ~Acquisition();
  Acquisition(const Acquisition&) = delete;
  Acquisition& operator=(const Acquisition&) = delete;
  Acquisition(Acquisition&&) = delete;
  Acquisition& operator=(Acquisition&&) = delete;

  persist::IAnalysisPersister& persister() noexcept;

 private:
  friend Result<std::unique_ptr<Acquisition>> open_acquisition(const std::string&, std::filesystem::path,
                                                               const std::string&);
  explicit Acquisition(std::unique_ptr<AcquisitionImpl> impl);

  std::unique_ptr<AcquisitionImpl> impl_;
  std::unique_ptr<persist::IAnalysisPersister> proxy_;  // marshals to impl_'s worker
};

// Opens `url` with migrate = false on the worker and registers the acquisition
// client. A failure to open or register is returned here, and no thread is
// left running. A schema that is behind is refused with the command that
// brings it up to date, naming the database (without its password). Call from
// the program's main thread: the Qt application object the store needs is made
// on the calling thread, not on the worker.
Result<std::unique_ptr<Acquisition>> open_acquisition(const std::string& url, std::filesystem::path data,
                                                      const std::string& software_version);

// Whether a simulated session saves its runs to the database `url`: when the
// operator said so (`sim_save`, --sim-save), or when the database is the
// chosen install's own (`from_db_option` false: it was not given with --db)
// and a file on this computer (`sqlite:`). Otherwise such a session writes
// record files, as it did before there was a database to write to: a
// simulator's runs do not belong among a lab's measurements, and a server, or
// a database somebody pointed the program at, is taken to hold those. The
// same rule in every program. A session that is not simulated is not asked.
bool saves_simulated_runs(std::string_view url, bool from_db_option, bool sim_save);

// `url` as it may be shown. A `sqlite:` url is shown as it is. A
// postgresql:// (or postgres://) url is shown without the password of its
// user (whatever it holds: `/`, `@`, `:`, encoded or not) and without a
// `password` or `sslpassword` parameter. Anything else is not understood,
// and is shown as `<database>`: a text that might hold a secret is not
// printed in the hope that it does not.
std::string without_password(const std::string& url);

// `text` (an error's, say) with `url` wherever it is quoted replaced by
// without_password(url), and the url's password, as written and decoded,
// by `***` (a password shorter than four characters is not looked for: it
// would be found in ordinary words).
std::string without_password_in(std::string text, const std::string& url);

}  // namespace pychron::experiment::store
