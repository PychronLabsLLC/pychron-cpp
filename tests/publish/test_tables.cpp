// The row tables of a release and their canonical text (data publishing
// design, sections 5.3 and 5.7).

#include "pychron/publish/tables.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstring>
#include <limits>

#include "processing/fixtures.hpp"

namespace pp = pychron::processing;
namespace ps = pychron::persistence;
using namespace pychron::publish;
using pp::test::make_step;
using pp::test::make_unknown;

namespace {

// Fixed ids, so two builds of the same inputs are the same bytes.
const ps::Uuid kNamespace = *ps::Uuid::parse("0192f3a0-0000-7000-8000-000000000001");

ps::SampleRow sample_row(const std::string& name, const std::string& uuid_seed, bool located) {
  ps::SampleRow s;
  s.uuid = ps::Uuid::v5(kNamespace, uuid_seed);
  s.name = name;
  s.project = ps::Uuid::v5(kNamespace, "project");
  s.material = ps::Uuid::v5(kNamespace, "material");
  s.project_name = "Fish Canyon";
  s.principal_investigator_name = "J Ross";
  s.material_name = "sanidine";
  s.grainsize = "60-80";
  if (located) {
    s.fields.lat = 37.75;
    s.fields.lon = -106.9;
    s.fields.elevation = 2850.0;
  }
  s.fields.igsn = "IEFC20001";
  s.updated = *ps::UtcTime::parse("2026-10-18T00:00:00Z");
  return s;
}

// Three steps of one aliquot of sample FC-2 and one fusion of sample U1,
// reduced and grouped by aliquot as the report groups them.
TableInputs inputs(bool fc2_located = true, bool u1_has_age = true) {
  TableInputs in;
  in.release.uuid = ps::Uuid::v5(kNamespace, "release");
  in.release.name = "r";
  in.release.version = 1;
  in.release.options_json = "{}";
  const ps::SampleRow fc2 = sample_row("FC-2", "fc2", fc2_located);
  const ps::SampleRow u1 = sample_row("U1 sample", "u1", true);
  in.samples = {u1, fc2};  // not in name order on purpose
  pp::Dataset d;
  const std::array<double, 3> ar39{20, 40, 60};
  const std::array<double, 3> ar36{0.3, 0.2, 0.1};
  for (int i = 0; i < 3; ++i) {
    auto a = make_step(i, ar39[i], ar36[i]);
    a->material = "sanidine";
    a->project = "Fish Canyon";
    a->irradiation = "NM-300";
    a->level = "A";
    a->position = "3";
    a->sample_info.latitude = fc2.fields.lat;
    a->sample_info.longitude = fc2.fields.lon;
    AnalysisPins pins;
    pins.analysis = ps::Uuid::v5(in.release.uuid, a->uuid);
    a->uuid = pins.analysis.str();  // as the store's source names an analysis
    pp::DatasetItem item;
    item.analysis = pp::reduce_analysis(a, {});
    item.path.group = 0;
    d.mutable_items().push_back(item);
    pins.sample = fc2.uuid;
    pins.heads[ps::Kind::Intercepts] = ps::Uuid::v5(pins.analysis, "intercepts");
    pins.heads[ps::Kind::Tags] = ps::Uuid::v5(pins.analysis, "tags");
    in.pins.push_back(pins);
  }
  {
    auto a = make_unknown(0);
    a->sample = "U1 sample";
    a->material = "sanidine";
    a->project = "Fish Canyon";
    a->timestamp = 1'700'000'000.0 - 600.0;  // before every step: it sorts first
    if (!u1_has_age) a->context.flux = std::nullopt;
    AnalysisPins pins;
    pins.analysis = ps::Uuid::v5(in.release.uuid, a->uuid);
    a->uuid = pins.analysis.str();
    pp::DatasetItem item;
    item.analysis = pp::reduce_analysis(a, {});
    item.path.group = 1;
    d.mutable_items().push_back(item);
    pins.sample = u1.uuid;
    in.pins.push_back(pins);
  }
  d.group_names = {"S1-01", "U1-01"};
  d.reduction_tag = "arar-1/default/d0";
  in.dataset = d;
  in.report = pp::make_report(d, {});
  return in;
}

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
}
