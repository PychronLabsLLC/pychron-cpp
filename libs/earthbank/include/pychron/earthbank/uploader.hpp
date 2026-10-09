// Sending submissions to EarthBank, once, and finishing after a failure.
//
// There is no sandbox and this client has no way to change or delete a
// record, so an upload is built around two facts: a record written twice is
// there twice, and a write whose answer never came may or may not have
// happened. For each submission (one per sample) it
//
//   1. runs the pre-flight check (preflight.hpp) over all of them and, if a
//      term is unknown, sends nothing and returns the misses;
//   2. reads the sample's journal (journal.hpp): the ids already given;
//   3. finds the sample in the data package by its name, or creates it with
//      its location;
//   4. creates the data point, then each aliquot, each measurement, the age
//      calculation and the age summary, each only if the journal does not
//      have it.
//
// The endpoints and bodies are the service's (legacy's, made strict):
//
//   GET  /api/core/sample-with-locations?name.equals=&dataPackageId.equals=
//   POST /api/core/sample-with-locations  {sampleDTO, locationDTO, dataPackageId, shortName}
//   POST /api/arar/ArArDataPoint          {dataPointDTO: {dataStructure "ARARDATAPOINT",
//                                          dataPackageId, sampleId, name},
//                                          extendingDataPointDTO, arArIrradiationName,
//                                          arArSetUpName}
//   POST /api/arar/ArArAliquot, ArArMeasurement, ArArAgeCalc, ArArAgeSummary
//                                         the record, with arArDataPointId
//
// Controlled terms go as their ids and their names are not sent; a term sent
// by name (the step flags, the lithology) goes by name.
//
// One record, in the journal:
//
//   not done --(in_doubt set, saved)--> POST --> answered 2xx: id recorded, in_doubt cleared
//                                          |--> answered otherwise, or never left: in_doubt cleared, not done
//                                          '--> no answer, a 5xx fault, a 2xx without an id:
//                                               in_doubt stays; the sample stops (InDoubt)
//
// A sample with a record in doubt gets nothing more until the user has looked
// for that record on EarthBank and says which it is (UploadOptions::
// resolve_in_doubt). The one record the uploader settles itself is the
// sample: it searches the package by name, as it does before creating one.
//
// A single thread, as the session. Progress and cancellation are called from
// the thread that calls upload().
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/earthbank/lookup.hpp"
#include "pychron/earthbank/model.hpp"
#include "pychron/earthbank/preflight.hpp"
#include "pychron/earthbank/session.hpp"

namespace pychron::earthbank {

// One step of one sample, reported after it is done (and once before the
// first, with `done` 0). `record` is the record's name as in Journal::in_doubt
// ("sample", "data point", "aliquot 20001-01", "measurement 3",
// "age calculation", "age summary"). `done` and `total` count the POSTs of
// this sample in this call: `total` is what is left to send when the sample
// has been found or is known to need creating.
struct UploadEvent {
  std::string sample, record, detail;
  std::size_t done = 0, total = 0;
};

struct UploadOptions {
  std::int64_t data_package_id = 0;
  // Send what the lookups know and leave out the terms they say EarthBank does
  // not have (listed in the sample's messages). A term the lookup could not
  // ask about is not covered: the service may know it.
  bool allow_unresolved = false;
  // The submission has changed since the journal was begun: put the old
  // journal aside and send the sample again, as new records.
  bool new_datapoint = false;
  // How the user settles the record a journal has in doubt, after looking on
  // EarthBank: "written:<id>" (it is there, with that id) or "not-written".
  // It names no sample, so it is used only when one sample is in doubt.
  std::optional<std::string> resolve_in_doubt;
  std::filesystem::path journal_dir;
  std::function<void(const UploadEvent&)> on_event;
  // Asked before each record is sent; true stops the sample there.
  std::function<bool()> cancelled;
};

enum class SampleStatus {
  Complete,  // everything is on EarthBank
  Partial,   // the data point is, and some of what hangs from it is not (`missing`)
  Stopped,   // cancelled, or a failure before the data point: nothing is in doubt
  InDoubt,   // a record may or may not be there: nothing more was sent
  Refused,   // not started: see `messages`
};

// `written`: the records this call sent and the service took (the record's
// name as in UploadEvent). `missing`: every record the journal does not have
// at the end, a record in doubt included. `messages` are sentences for the
// user.
struct SampleReport {
  std::string sample;
  SampleStatus status = SampleStatus::Refused;
  std::optional<std::int64_t> data_point_id;
  std::vector<std::string> written, missing, messages;
};

// `misses` are the pre-flight's, when they stopped the upload; every sample
// is then Refused and nothing was sent.
struct UploadReport {
  std::vector<SampleReport> samples;
  std::vector<Miss> misses;
};

// An error (nothing was sent): no data package, no login, an unusable
// `resolve_in_doubt` or no journal directory. What happens to a sample is in
// its report.
Result<UploadReport> upload(const std::vector<Submission>& submissions, Session& session, LookupResolver& resolver,
                            const UploadOptions& options);

}  // namespace pychron::earthbank
