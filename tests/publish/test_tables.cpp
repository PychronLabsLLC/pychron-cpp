// The row tables of a release and their canonical text (data publishing
// design, sections 5.3 and 5.7).

#include "pychron/publish/tables.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstring>
#include <limits>

#include "table_inputs.hpp"

namespace ps = pychron::persistence;
using namespace pychron::publish;

namespace {

using pychron::publish::testing::inputs;

const Cell& cell(const Table& t, std::size_t row, const char* column) {
  const auto c = t.column(column);
  EXPECT_TRUE(c) << column;
  return t.rows.at(row).at(c.value_or(0));
}

bool is_null(const Cell& c) { return std::holds_alternative<std::monostate>(c); }

}  // namespace

TEST(Tables, AnalysesColumnsMatchTheSpecOrder) {
  const auto t = build_tables(inputs()).analyses;
  ASSERT_FALSE(t.columns.empty());
  EXPECT_EQ(t.columns.front().name, "analysis_uuid");
  EXPECT_EQ(t.columns.back().name, "bbox");
  EXPECT_TRUE(t.geo);
  const std::vector<std::string> expected{
      "analysis_uuid", "runid", "identifier", "aliquot", "increment", "step", "sample_uuid", "sample", "project",
      "material", "analysis_type", "experiment_type", "timestamp_utc", "mass_spectrometer", "extract_device",
      "analyst", "laboratory", "extract_value", "extract_units", "extract_duration_s", "cleanup_duration_s",
      "weight_mg", "beam_diameter", "pattern", "irradiation", "level", "position", "tag", "included", "group_id",
      "comment", "j", "j_err", "ar40_fa", "ar40_fa_err", "ar39_fa", "ar39_fa_err", "ar38_fa", "ar38_fa_err",
      "ar37_fa", "ar37_fa_err", "ar36_fa", "ar36_fa_err", "ar40_blank_fa", "ar40_blank_fa_err", "ar39_blank_fa",
      "ar39_blank_fa_err", "ar38_blank_fa", "ar38_blank_fa_err", "ar37_blank_fa", "ar37_blank_fa_err",
      "ar36_blank_fa", "ar36_blank_fa_err", "f", "f_err", "rad40_pct", "k39_fa", "k39_pct_group", "k39_cum_pct",
      "kca", "kca_err", "kcl", "kcl_err", "iso_39_40", "iso_39_40_err", "iso_36_40", "iso_36_40_err", "iso_corr",
      "age_ma", "age_err_analytical_ma", "age_err_with_j_ma", "age_note", "record_sha256", "rev_signals",
      "rev_intercepts", "rev_baselines", "rev_blanks", "rev_icfactors", "rev_tags", "rev_annotation", "geometry",
      "bbox"};
  std::vector<std::string> names;
  names.reserve(t.columns.size());
  for (const auto& c : t.columns) names.push_back(c.name);
  EXPECT_EQ(names, expected);
  EXPECT_EQ(t.column("timestamp_utc").has_value(), true);
  EXPECT_EQ(t.columns[*t.column("timestamp_utc")].type, ColumnType::TimestampUtc);
  EXPECT_EQ(t.columns[*t.column("aliquot")].type, ColumnType::Int32);
  EXPECT_EQ(t.columns[*t.column("included")].type, ColumnType::Bool);
  EXPECT_EQ(t.columns[*t.column("geometry")].type, ColumnType::Wkb);
  EXPECT_EQ(t.columns[*t.column("bbox")].type, ColumnType::Json);
}

TEST(Tables, RowsAreOrderedByTimestampThenUuid) {
  const auto tables = build_tables(inputs());
  const auto& t = tables.analyses;
  ASSERT_EQ(t.rows.size(), 4u);
  EXPECT_EQ(std::get<std::string>(cell(t, 0, "identifier")), "U1");
  EXPECT_EQ(std::get<std::string>(cell(t, 1, "runid")), "S1-01A");
  EXPECT_EQ(std::get<std::string>(cell(t, 3, "runid")), "S1-01C");
  EXPECT_EQ(std::get<std::int32_t>(cell(t, 1, "increment")), 0);
  EXPECT_EQ(std::get<std::string>(cell(t, 1, "step")), "A");
  EXPECT_EQ(std::get<UtcTime>(cell(t, 1, "timestamp_utc")).iso(), "2023-11-14T22:13:20.000000Z");
  EXPECT_TRUE(std::get<bool>(cell(t, 1, "included")));
  EXPECT_EQ(std::get<std::string>(cell(t, 1, "group_id")), "S1-01");
  // Samples by name: "FC-2" before "U1 sample".
  EXPECT_EQ(std::get<std::string>(cell(tables.samples, 0, "name")), "FC-2");
  EXPECT_EQ(std::get<std::int32_t>(cell(tables.samples, 0, "n_analyses")), 3);
  EXPECT_EQ(std::get<std::int32_t>(cell(tables.samples, 0, "n_ages")), 3);
  EXPECT_EQ(std::get<std::string>(cell(tables.samples, 0, "igsn")), "IEFC20001");
}

TEST(Tables, AnalysisValuesComeFromTheReport) {
  const auto tables = build_tables(inputs());
  const auto& t = tables.analyses;
  const auto f = std::get<double>(cell(t, 1, "f"));
  EXPECT_NEAR(f, 10.0, 1e-6);  // make_step: every step has F = 10
  EXPECT_FALSE(is_null(cell(t, 1, "age_ma")));
  EXPECT_FALSE(is_null(cell(t, 1, "age_err_analytical_ma")));
  EXPECT_FALSE(is_null(cell(t, 1, "ar40_fa")));
  EXPECT_FALSE(is_null(cell(t, 1, "k39_cum_pct")));
  EXPECT_NEAR(std::get<double>(cell(t, 3, "k39_cum_pct")), 100.0, 1e-9);
  EXPECT_NEAR(std::get<double>(cell(t, 1, "j")), 0.001, 1e-12);
  // Pins land in the rev_* columns; kinds without a pin are null.
  EXPECT_FALSE(is_null(cell(t, 1, "rev_intercepts")));
  EXPECT_TRUE(is_null(cell(t, 1, "rev_blanks")));
  EXPECT_TRUE(is_null(cell(t, 1, "record_sha256")));
}

TEST(Tables, SampleWithoutLocationHasNullGeometry) {
  const auto tables = build_tables(inputs(false));
  const auto& s = tables.samples;
  EXPECT_TRUE(is_null(cell(s, 0, "geometry")));
  EXPECT_TRUE(is_null(cell(s, 0, "bbox")));
  EXPECT_FALSE(is_null(cell(s, 1, "geometry")));
  EXPECT_EQ(std::get<JsonText>(cell(s, 1, "bbox")).value, R"({"xmin":-106.9,"ymin":37.75,"xmax":-106.9,"ymax":37.75})");
  const auto& a = tables.analyses;
  EXPECT_TRUE(is_null(cell(a, 1, "geometry")));  // FC-2's step
  EXPECT_FALSE(is_null(cell(a, 0, "geometry")));  // U1's fusion
}

TEST(Tables, AnalysisWithoutAgeHasNullAgeColumns) {
  const auto tables = build_tables(inputs(true, false));
  const auto& a = tables.analyses;
  EXPECT_TRUE(is_null(cell(a, 0, "age_ma")));
  EXPECT_TRUE(is_null(cell(a, 0, "age_err_with_j_ma")));
  EXPECT_FALSE(is_null(cell(a, 0, "age_note")));
  EXPECT_FALSE(std::get<std::string>(cell(a, 0, "age_note")).empty());
  EXPECT_EQ(std::get<std::int32_t>(cell(tables.samples, 1, "n_analyses")), 1);
  EXPECT_EQ(std::get<std::int32_t>(cell(tables.samples, 1, "n_ages")), 0);
}

TEST(Tables, IsotopeRowsPerAnalysisAndIsotope) {
  const auto tables = build_tables(inputs());
  const auto& t = tables.isotopes;
  EXPECT_FALSE(t.geo);
  ASSERT_EQ(t.rows.size(), 4u * 5u);
  EXPECT_EQ(std::get<std::string>(cell(t, 0, "units")), "fA");
  EXPECT_EQ(std::get<std::string>(cell(t, 0, "isotope")), "Ar40");
  EXPECT_EQ(std::get<std::string>(cell(t, 4, "isotope")), "Ar36");
  EXPECT_FALSE(is_null(cell(t, 0, "intercept")));
  EXPECT_FALSE(is_null(cell(t, 0, "corrected")));
  EXPECT_TRUE(is_null(cell(t, 0, "blob_sha256")));
  EXPECT_EQ(std::get<std::string>(cell(t, 0, "analysis_uuid")),
            std::get<std::string>(cell(tables.analyses, 0, "analysis_uuid")));
}

TEST(Tables, PositionsOnePerIdentifierFromTheReport) {
  const auto tables = build_tables(inputs());
  const auto& t = tables.positions;
  ASSERT_EQ(t.rows.size(), 2u);
  EXPECT_EQ(std::get<std::string>(cell(t, 0, "identifier")), "S1");  // NM-300/A/3 before the un-irradiated U1
  EXPECT_EQ(std::get<std::string>(cell(t, 0, "irradiation")), "NM-300");
  EXPECT_EQ(std::get<std::int32_t>(cell(t, 0, "position")), 3);
  EXPECT_NEAR(std::get<double>(cell(t, 0, "j")), 0.001, 1e-12);
  EXPECT_TRUE(is_null(cell(t, 0, "flux_revision_uuid")));
}

TEST(Tables, InterpretedAgesCarryMembersAsJson) {
  auto in = inputs();
  InterpretedAgeRecord ia;
  ia.ia = ps::Uuid::v7();
  ia.revision = ps::Uuid::v7();
  ia.name = "FC-2 plateau";
  ia.sample = in.samples[1].uuid;
  ia.identifier = "S1";
  ia.value.age = 28.2;
  ia.value.age_err = 0.1;
  ia.value.age_kind = "Plateau";
  ia.value.mswd = 1.1;
  ia.value.nanalyses = 3;
  ia.value.members = {{in.pins[0].analysis, std::nullopt, true, "ok"}};
  in.interpreted_ages = {ia};
  const auto t = build_tables(in).interpreted_ages;
  ASSERT_EQ(t.rows.size(), 1u);
  EXPECT_TRUE(t.geo);
  EXPECT_EQ(std::get<std::string>(cell(t, 0, "age_kind")), "plateau");
  EXPECT_EQ(std::get<std::int32_t>(cell(t, 0, "n")), 3);
  EXPECT_EQ(std::get<JsonText>(cell(t, 0, "members")).value,
            R"([{"analysis_uuid":")" + in.pins[0].analysis.str() + R"(","plateau_step":true,"tag":"ok"}])");
  EXPECT_FALSE(is_null(cell(t, 0, "geometry")));
}

TEST(Tables, CanonicalTextOfKnownRowIsExact) {
  const std::vector<Cell> row{1.5, std::int32_t{2}, true, std::string{"a,b"},
                              *UtcTime::parse("2026-10-18T00:00:00Z"), std::monostate{}};
  EXPECT_EQ(canonical_row_text(row), std::string("1.5\x1f") + "2\x1f" + "true\x1f" + "a,b\x1f" +
                                         "2026-10-18T00:00:00.000000Z\x1f" + "");
  EXPECT_EQ(canonical_double(100.0), "100");
  EXPECT_EQ(canonical_double(1e-7), "1e-07");
  EXPECT_EQ(canonical_double(0.1), "0.1");
  EXPECT_EQ(canonical_double(-2.5e20), "-2.5e+20");
}

TEST(Tables, NonFiniteDoubleIsNull) {
  const std::vector<Cell> row{std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()};
  EXPECT_EQ(canonical_row_text(row), "\x1f");
  auto in = inputs();
  // A sample elevation that is not a number is written as null.
  in.samples[0].fields.elevation = std::numeric_limits<double>::quiet_NaN();
  const auto t = build_tables(in).samples;
  EXPECT_TRUE(is_null(cell(t, 1, "elevation_m")));
}

TEST(Tables, RowsSha256IsStableAcrossTwoBuilds) {
  const auto a = build_tables(inputs());
  const auto b = build_tables(inputs());
  EXPECT_EQ(rows_sha256(a.analyses), rows_sha256(b.analyses));
  EXPECT_EQ(rows_sha256(a.isotopes), rows_sha256(b.isotopes));
  EXPECT_NE(rows_sha256(a.analyses), rows_sha256(a.isotopes));
  Table empty;
  EXPECT_EQ(pychron::to_hex(rows_sha256(empty)), pychron::to_hex(pychron::sha256(std::string_view{})));
}

TEST(Tables, WkbPointIsTwentyOneBytesLittleEndian) {
  const std::string wkb = wkb_point(-106.9, 34.0);
  ASSERT_EQ(wkb.size(), 21u);
  EXPECT_EQ(static_cast<unsigned char>(wkb[0]), 0x01);
  EXPECT_EQ(static_cast<unsigned char>(wkb[1]), 0x01);
  EXPECT_EQ(static_cast<unsigned char>(wkb[2]), 0x00);
  EXPECT_EQ(static_cast<unsigned char>(wkb[3]), 0x00);
  EXPECT_EQ(static_cast<unsigned char>(wkb[4]), 0x00);
  double x = 0, y = 0;
  std::memcpy(&x, wkb.data() + 5, 8);
  std::memcpy(&y, wkb.data() + 13, 8);
  EXPECT_EQ(x, -106.9);
  EXPECT_EQ(y, 34.0);
}

TEST(Tables, FileSafeReplacesEverythingButWordChars) {
  EXPECT_EQ(file_safe("FC-2 / split (b)"), "FC-2___split__b_");
  EXPECT_EQ(file_safe("ok.name_1"), "ok.name_1");
  EXPECT_EQ(file_safe("ümlaut"), "__mlaut");  // two UTF-8 bytes
  EXPECT_EQ(file_safe(".."), "sample");
  EXPECT_EQ(file_safe("."), "sample");
  EXPECT_EQ(file_safe(""), "sample");
  EXPECT_EQ(file_safe("CON"), "CON_");
  EXPECT_EQ(file_safe("trailing."), "trailing_");
}

// The row hashes of the fixed in-memory inputs, against the committed
// fixture (tests/publish/fixtures/README.md): what Windows and Linux CI
// compare with a file written on macOS. PYCHRON_WRITE_FIXTURE=1 rewrites it.
TEST(Tables, RowHashesEqualTheCommittedFixture) {
  const auto tables = build_tables(inputs());
  std::string actual;
  for (const Table* t : {&tables.samples, &tables.positions, &tables.analyses, &tables.isotopes, &tables.interpreted_ages})
    actual += t->name + " " + std::to_string(t->rows.size()) + " " + pychron::to_hex(rows_sha256(*t)) + "\n";
  const std::filesystem::path fixture = std::filesystem::path(PYCHRON_PUBLISH_FIXTURE_DIR) / "table-inputs.rows_sha256";
  if (const char* write = std::getenv("PYCHRON_WRITE_FIXTURE"); write && std::string(write) == "1") {
    std::ofstream out(fixture, std::ios::binary);
    out << actual;
    GTEST_SKIP() << "wrote " << fixture;
  }
  std::ifstream in(fixture, std::ios::binary);
  ASSERT_TRUE(in) << "no fixture at " << fixture << "; write it with PYCHRON_WRITE_FIXTURE=1";
  std::stringstream expected;
  expected << in.rdbuf();
  EXPECT_EQ(actual, expected.str()) << "the tables' rows differ from the committed fixture";
}

// Every column of every table is documented in docs/release_format.md, in
// a backticked span (the reader's reference; Task 11 of the phase 1 plan).
TEST(Tables, EveryColumnIsDocumented) {
  const std::filesystem::path doc = std::filesystem::path(PYCHRON_SOURCE_DIR) / "docs" / "release_format.md";
  std::ifstream in(doc, std::ios::binary);
  ASSERT_TRUE(in) << doc;
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string text = ss.str();
  const auto tables = build_tables(inputs());
  for (const Table* t : {&tables.samples, &tables.positions, &tables.analyses, &tables.isotopes, &tables.interpreted_ages})
    for (const auto& c : t->columns)
      EXPECT_NE(text.find("`" + c.name + "`"), std::string::npos) << t->name << "." << c.name << " is not documented";
}
