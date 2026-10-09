// What an upload knows about what it has already put on EarthBank.
//
// EarthBank offers this client no way to change or take back a record, and a
// write whose answer never came may or may not have happened. So an upload
// keeps a journal for each sample it sends to a data package, on this
// machine, and the journal is the only memory a re-run has:
//
//   - the ids the service gave: the sample, the data point, each aliquot, each
//     measurement, the age calculation, the age summary;
//   - a hash of the submission (content_hash), so that a re-run of changed
//     data is noticed;
//   - `in_doubt`: the record whose POST was about to be sent or has no
//     answer. It is saved before the request is made and cleared, with the id
//     recorded, when the answer comes. A journal that still has it says "this
//     record may be on EarthBank"; nothing is sent for the sample until the
//     user has looked and said which it is (uploader.hpp).
//
// The journal is one JSON file per (host, data package, sample):
//
//   <journal dir>/<host>/<package id>/<sample>.json
//
//   {
//     "version": 1,
//     "hash": "9f2c...e1",
//     "sampleId": 710001,
//     "arArDataPointId": 740001,
//     "aliquots": {"20001-01": 750001},
//     "measurements": {"0001": 760001, "0002": 760002},
//     "ageCalcId": 770001,
//     "inDoubt": "age summary"
//   }
//
// A measurement is named by its place in the submission (1-based, at least
// four digits): a measurement has no field that tells it from the others of
// its aliquot, and the hash fixes the order. File names are the host and the
// sample percent-encoded (journal_file), so that a name never reaches outside
// the directory or collides with another name.
//
// A file is replaced whole (written beside, flushed to the disk, renamed,
// the directory flushed), so a reader finds the journal as it was before the
// save or as it is after it, never half of one. A file that cannot be read
// back is an error, never "no journal": that would let a re-run send
// everything again. There are no secrets in a journal.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "pychron/core/error.hpp"

namespace pychron::earthbank {

struct Journal {
  std::string hash;
  std::optional<std::int64_t> sample_id, data_point_id, age_calc_id, age_summary_id;
  std::map<std::string, std::int64_t> aliquots, measurements;  // by aliquot name, by place ("0001")
  std::optional<std::string> in_doubt;                          // the record whose POST had no answer

  // Nothing is recorded in it: no id, no record in doubt.
  bool empty() const {
    return !sample_id && !data_point_id && !age_calc_id && !age_summary_id && aliquots.empty() &&
           measurements.empty() && !in_doubt;
  }
  bool operator==(const Journal&) const = default;
};

// <dir>/<host>/<package>/<sample>.json. `host` is the service's host (and
// port, if any), `sample` its name as the lab writes it. Anything outside
// ASCII letters, digits, `-`, `_` and `.` is `%XX`, and so is a leading `.`
// (no name is `.` or `..`); a name too long for a file system is cut and
// ends in a hash of the whole name.
std::filesystem::path journal_file(const std::filesystem::path& dir, std::string_view host, std::int64_t package,
                                   std::string_view sample);

// No file: no journal (nullopt). A file that is not a journal of this program
// (not JSON, an unknown key, a wrong type, a version it does not know) is a
// `Config` error naming the file; one that cannot be read is an `Io` error.
Result<std::optional<Journal>> load_journal(const std::filesystem::path& file);

// Atomic: the file is the old journal or the new one. Makes the directory.
Result<void> save_journal(const Journal& journal, const std::filesystem::path& file);

}  // namespace pychron::earthbank
