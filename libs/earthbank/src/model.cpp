#include "pychron/earthbank/model.hpp"

#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/sha256.hpp"

namespace pychron::earthbank {
namespace {

template <typename T>
void put(nlohmann::json& out, const char* name, const std::optional<T>& field) {
  if (field) out[name] = *field;
}

// A number that is not finite has no JSON form; it is not sent.
void put(nlohmann::json& out, const char* name, const Number& field) {
  if (field && std::isfinite(*field)) out[name] = *field;
}

// An empty string tells the service nothing; it is not sent.
void put(nlohmann::json& out, const char* name, const Text& field) {
  if (field && !field->empty()) out[name] = *field;
}

template <typename Record>
nlohmann::json array_of(const std::vector<Record>& records) {
  nlohmann::json out = nlohmann::json::array();
  for (const Record& record : records) out.push_back(to_json(record));
  return out;
}

}  // namespace

#define PYCHRON_EARTHBANK_PUT(kind, name) put(out, #name, record.name);
#define PYCHRON_EARTHBANK_TO_JSON(Record, FIELDS)   \
  nlohmann::json to_json(const Record& record) {    \
    nlohmann::json out = nlohmann::json::object();  \
    FIELDS(PYCHRON_EARTHBANK_PUT)                   \
    return out;                                     \
  }

PYCHRON_EARTHBANK_TO_JSON(Sample, PYCHRON_EARTHBANK_SAMPLE_FIELDS)
PYCHRON_EARTHBANK_TO_JSON(Location, PYCHRON_EARTHBANK_LOCATION_FIELDS)
PYCHRON_EARTHBANK_TO_JSON(DataPoint, PYCHRON_EARTHBANK_DATA_POINT_FIELDS)
PYCHRON_EARTHBANK_TO_JSON(Aliquot, PYCHRON_EARTHBANK_ALIQUOT_FIELDS)
PYCHRON_EARTHBANK_TO_JSON(Measurement, PYCHRON_EARTHBANK_MEASUREMENT_FIELDS)
PYCHRON_EARTHBANK_TO_JSON(AgeCalc, PYCHRON_EARTHBANK_AGE_CALC_FIELDS)
PYCHRON_EARTHBANK_TO_JSON(AgeSummary, PYCHRON_EARTHBANK_AGE_SUMMARY_FIELDS)

#undef PYCHRON_EARTHBANK_TO_JSON
#undef PYCHRON_EARTHBANK_PUT

std::string content_hash(const Submission& submission) {
  nlohmann::json records = nlohmann::json::array();
  records.push_back(to_json(submission.sample));
  records.push_back(submission.location ? to_json(*submission.location) : nlohmann::json(nullptr));
  records.push_back(to_json(submission.data_point));
  records.push_back(array_of(submission.aliquots));
  records.push_back(array_of(submission.measurements));
  records.push_back(to_json(submission.age_calc));
  records.push_back(to_json(submission.age_summary));
  const Sha256Digest digest = sha256(records.dump());
  return to_hex(std::span<const std::uint8_t>(digest));
}

}  // namespace pychron::earthbank
