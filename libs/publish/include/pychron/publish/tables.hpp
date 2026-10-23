#pragma once

// The row tables of a release (data publishing design, section 5.3): what
// data/*.parquet hold, as plain columns and cells, and the canonical text of
// a row that the manifest's rows_sha256 hashes (section 5.7). Independent of
// the Parquet encoder: a reader of another version verifies rows, not bytes.
//
// Values are read from what the build already computed: the Schaen report's
// cells (report.hpp) for the analyses and positions, the reduced analyses
// for the isotopes, the catalog rows for the samples. Nothing is reduced a
// second time here.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "pychron/core/sha256.hpp"
#include "pychron/persistence/catalog.hpp"
#include "pychron/persistence/ids.hpp"
#include "pychron/persistence/model.hpp"
#include "pychron/persistence/publish.hpp"
#include "pychron/persistence/store.hpp"
#include "pychron/processing/dataset.hpp"
#include "pychron/arar/report.hpp"

namespace pychron::publish {

using persistence::Uuid;
using persistence::UtcTime;

enum class ColumnType { Double, Int32, Int64, Bool, String, TimestampUtc, Json, Wkb };

struct Column {
  std::string name;
  ColumnType type = ColumnType::String;
  bool nullable = true;
};

// Canonical JSON text (a list or a struct), written by the build.
struct JsonText {
  std::string value;
  friend bool operator==(const JsonText&, const JsonText&) = default;
};
// Well-known binary of a geometry.
struct WkbBytes {
  std::string value;
  friend bool operator==(const WkbBytes&, const WkbBytes&) = default;
};

// A cell: nothing (null), or a value of the column's type. A non-finite
// double is written as null, never as a number.
using Cell = std::variant<std::monostate, double, std::int32_t, std::int64_t, bool, std::string, UtcTime, JsonText,
                          WkbBytes>;

struct Table {
  std::string name;  // samples, positions, analyses, isotopes, interpreted_ages
  std::vector<Column> columns;
  std::vector<std::vector<Cell>> rows;  // every row has columns.size() cells
  bool geo = false;                     // carries a `geometry` (WKB point) and a `bbox` struct column

  std::optional<std::size_t> column(std::string_view column_name) const;
};

struct ReleaseTables {
  Table samples, positions, analyses, isotopes, interpreted_ages;
};

// ---------------------------------------------------------------- inputs

// What a release pins of one analysis (spec 5.7 "pins"), with the payloads
// the isotope columns are filled from. `refs` are the resolved references at
// the pinned revisions; `signals` and `intercepts` the pinned payloads of
// those kinds (empty when the build did not read them: the columns are null).
struct AnalysisPins {
  Uuid analysis;
  std::optional<Uuid> sample;  // the analysis's sample row (the identifier's own, else its position's)
  std::map<persistence::Kind, Uuid> heads;
  std::vector<persistence::ResolvedRef> refs;
  std::optional<Sha256Digest> record_sha256;
  persistence::SignalRefs signals;
  persistence::Intercepts intercepts;
};

struct InterpretedAgeRecord {
  Uuid ia, revision;
  std::string name;
  Uuid sample;
  std::string identifier;
  persistence::InterpretedAgeValue value;
};

// Inputs the tables are made from. `dataset` is reduced and grouped as the
// report was, and `report` is make_report of it with the release's options.
struct TableInputs {
  persistence::ReleaseRow release;
  processing::Dataset dataset;
  processing::Report report;
  std::vector<persistence::SampleRow> samples;  // the release's samples (release_sample), by uuid
  std::vector<InterpretedAgeRecord> interpreted_ages;
  std::vector<AnalysisPins> pins;  // one per member, by analysis
};

// The five tables, columns and order exactly as section 5.3: samples by
// name then uuid; positions by irradiation, level, position; analyses by
// timestamp then uuid; isotopes by analysis order then isotope; interpreted
// ages by sample then name.
ReleaseTables build_tables(const TableInputs& in);

// ---------------------------------------------------------------- canonical text (5.7)

// A double as its shortest round-trip decimal text ("1.5", "1e-07", "100").
std::string canonical_double(double value);
// The cells joined by \x1f: a double as canonical_double, an integer in
// decimal, a bool "true"/"false", text as it is, a time as ISO-8601 UTC with
// microseconds, JSON as it is, WKB as lower-case hex, null as "".
std::string canonical_row_text(const std::vector<Cell>& row);
// SHA-256 of the rows' canonical texts joined by \x1e, in row order.
Sha256Digest rows_sha256(const Table& table);

// Little-endian WKB of POINT(lon lat): 21 bytes.
std::string wkb_point(double lon, double lat);
// `name` with every character outside [A-Za-z0-9._-] replaced by '_'.
std::string file_safe(std::string_view name);

}  // namespace pychron::publish
