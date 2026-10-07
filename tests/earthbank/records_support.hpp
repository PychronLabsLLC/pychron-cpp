// Records with every field set, for tests that walk a record's fields.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "pychron/earthbank/model.hpp"

namespace pychron::earthbank::testing {

inline void fill(std::optional<double>& field) { field = 1.5; }
inline void fill(std::optional<std::string>& field) { field = "x"; }
inline void fill(std::optional<std::int64_t>& field) { field = 7; }
inline void fill(std::optional<bool>& field) { field = true; }

#define PYCHRON_EARTHBANK_FILL(kind, name) fill(record.name);
#define PYCHRON_EARTHBANK_ALL_SET(Record, FIELDS) \
  inline Record all_set(const Record&) {          \
    Record record;                                \
    FIELDS(PYCHRON_EARTHBANK_FILL)                \
    return record;                                \
  }

PYCHRON_EARTHBANK_ALL_SET(Sample, PYCHRON_EARTHBANK_SAMPLE_FIELDS)
PYCHRON_EARTHBANK_ALL_SET(Location, PYCHRON_EARTHBANK_LOCATION_FIELDS)
PYCHRON_EARTHBANK_ALL_SET(DataPoint, PYCHRON_EARTHBANK_DATA_POINT_FIELDS)
PYCHRON_EARTHBANK_ALL_SET(Aliquot, PYCHRON_EARTHBANK_ALIQUOT_FIELDS)
PYCHRON_EARTHBANK_ALL_SET(Measurement, PYCHRON_EARTHBANK_MEASUREMENT_FIELDS)
PYCHRON_EARTHBANK_ALL_SET(AgeCalc, PYCHRON_EARTHBANK_AGE_CALC_FIELDS)
PYCHRON_EARTHBANK_ALL_SET(AgeSummary, PYCHRON_EARTHBANK_AGE_SUMMARY_FIELDS)

#undef PYCHRON_EARTHBANK_ALL_SET
#undef PYCHRON_EARTHBANK_FILL

// to_json of a `Record` with every field set: all the keys it can emit.
template <typename Record>
nlohmann::json all_fields() {
  return to_json(all_set(Record{}));
}

}  // namespace pychron::earthbank::testing
