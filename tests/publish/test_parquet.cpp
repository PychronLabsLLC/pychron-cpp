// GeoParquet tables (data publishing design, section 5.2). The Arrow cases
// run only in a build with Arrow (PYCHRON_PUBLISH); the last one only without.

#include "pychron/publish/parquet.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include "table_inputs.hpp"

#ifdef PYCHRON_PUBLISH_ARROW_ENABLED
#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>
#endif

namespace fs = std::filesystem;
using namespace pychron;
using namespace pychron::publish;
using pychron::publish::testing::inputs;

namespace {

fs::path scratch(const char* name) {
  const auto dir = fs::temp_directory_path() / ("pychron_parquet_" + std::to_string(std::random_device{}()));
  fs::create_directories(dir);
  return dir / name;
}

std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

#ifdef PYCHRON_PUBLISH_ARROW_ENABLED
std::shared_ptr<arrow::Table> read_back(const fs::path& path) {
  auto infile = arrow::io::ReadableFile::Open(path.string());
  EXPECT_TRUE(infile.ok()) << infile.status().ToString();
  if (!infile.ok()) return nullptr;
  auto reader = parquet::arrow::OpenFile(*infile, arrow::default_memory_pool());
  EXPECT_TRUE(reader.ok()) << reader.status().ToString();
  if (!reader.ok()) return nullptr;
  auto table = (*reader)->ReadTable();
  EXPECT_TRUE(table.ok()) << table.status().ToString();
  return table.ok() ? *table : nullptr;
}
#endif

}  // namespace

TEST(Parquet, ReadsBackColumnsAndTypes) {
  auto writer = make_parquet_writer();
  if (!writer) GTEST_SKIP() << "built without Arrow";
#ifdef PYCHRON_PUBLISH_ARROW_ENABLED
  const auto tables = build_tables(inputs());
  const fs::path path = scratch("analyses.parquet");
  auto written = writer->write(tables.analyses, path);
  ASSERT_TRUE(written) << to_string(written.error());
  auto table = read_back(path);
  ASSERT_TRUE(table);
  EXPECT_EQ(static_cast<std::size_t>(table->num_rows()), tables.analyses.rows.size());
  ASSERT_EQ(static_cast<std::size_t>(table->num_columns()), tables.analyses.columns.size());
  const auto& schema = *table->schema();
  for (std::size_t i = 0; i < tables.analyses.columns.size(); ++i)
    EXPECT_EQ(schema.field(static_cast<int>(i))->name(), tables.analyses.columns[i].name);
  EXPECT_EQ(schema.GetFieldByName("timestamp_utc")->type()->id(), arrow::Type::TIMESTAMP);
  EXPECT_EQ(schema.GetFieldByName("aliquot")->type()->id(), arrow::Type::INT32);
  EXPECT_EQ(schema.GetFieldByName("included")->type()->id(), arrow::Type::BOOL);
  EXPECT_EQ(schema.GetFieldByName("f")->type()->id(), arrow::Type::DOUBLE);
  EXPECT_EQ(schema.GetFieldByName("runid")->type()->id(), arrow::Type::STRING);
  EXPECT_EQ(schema.GetFieldByName("geometry")->type()->id(), arrow::Type::BINARY);
  EXPECT_EQ(schema.GetFieldByName("bbox")->type()->id(), arrow::Type::STRUCT);
  const auto ts = std::static_pointer_cast<arrow::TimestampType>(schema.GetFieldByName("timestamp_utc")->type());
  EXPECT_EQ(ts->unit(), arrow::TimeUnit::MICRO);
  EXPECT_EQ(ts->timezone(), "UTC");
  // The bbox struct carries xmin, ymin, xmax, ymax as doubles.
  const auto bbox = std::static_pointer_cast<arrow::StructType>(schema.GetFieldByName("bbox")->type());
  ASSERT_EQ(bbox->num_fields(), 4);
  EXPECT_EQ(bbox->field(0)->name(), "xmin");
  EXPECT_EQ(bbox->field(3)->name(), "ymax");
  EXPECT_EQ(bbox->field(0)->type()->id(), arrow::Type::DOUBLE);
  fs::remove_all(path.parent_path());
#endif
}

TEST(Parquet, GeoMetadataIsGeoParquet11) {
  auto writer = make_parquet_writer();
  if (!writer) GTEST_SKIP() << "built without Arrow";
#ifdef PYCHRON_PUBLISH_ARROW_ENABLED
  const auto tables = build_tables(inputs());
  const fs::path path = scratch("samples.parquet");
  ASSERT_TRUE(writer->write(tables.samples, path));
  auto table = read_back(path);
  ASSERT_TRUE(table);
  const auto metadata = table->schema()->metadata();
  ASSERT_TRUE(metadata);
  auto geo = metadata->Get("geo");
  ASSERT_TRUE(geo.ok()) << "no geo metadata";
  const auto j = nlohmann::json::parse(*geo);
  EXPECT_EQ(j.at("version"), "1.1.0");
  EXPECT_EQ(j.at("primary_column"), "geometry");
  const auto& g = j.at("columns").at("geometry");
  EXPECT_EQ(g.at("encoding"), "WKB");
  EXPECT_EQ(g.at("geometry_types")[0], "Point");
  EXPECT_EQ(g.at("crs").at("id").at("authority"), "OGC");
  EXPECT_EQ(g.at("crs").at("id").at("code"), "CRS84");
  EXPECT_EQ(g.at("covering").at("bbox").at("xmin")[0], "bbox");
  EXPECT_EQ(g.at("covering").at("bbox").at("xmin")[1], "xmin");
  EXPECT_EQ(g.at("covering").at("bbox").at("ymax")[1], "ymax");
  ASSERT_EQ(g.at("bbox").size(), 4u);
  EXPECT_DOUBLE_EQ(g.at("bbox")[0].get<double>(), -106.9);
  EXPECT_DOUBLE_EQ(g.at("bbox")[1].get<double>(), 37.75);
  // A table without geometry carries no geo key.
  const fs::path plain = scratch("isotopes.parquet");
  ASSERT_TRUE(writer->write(tables.isotopes, plain));
  auto iso = read_back(plain);
  ASSERT_TRUE(iso);
  EXPECT_TRUE(!iso->schema()->metadata() || !iso->schema()->metadata()->Contains("geo"));
  fs::remove_all(path.parent_path());
  fs::remove_all(plain.parent_path());
#endif
}

TEST(Parquet, NullsStayNull) {
  auto writer = make_parquet_writer();
  if (!writer) GTEST_SKIP() << "built without Arrow";
#ifdef PYCHRON_PUBLISH_ARROW_ENABLED
  const auto tables = build_tables(inputs(false));  // FC-2 has no location
  const fs::path path = scratch("samples.parquet");
  ASSERT_TRUE(writer->write(tables.samples, path));
  auto table = read_back(path);
  ASSERT_TRUE(table);
  const auto geometry = table->GetColumnByName("geometry");
  ASSERT_TRUE(geometry);
  EXPECT_EQ(geometry->null_count(), 1);
  const auto bbox = table->GetColumnByName("bbox");
  ASSERT_TRUE(bbox);
  EXPECT_EQ(bbox->null_count(), 1);
  const auto igsn = table->GetColumnByName("igsn");
  ASSERT_TRUE(igsn);
  EXPECT_EQ(igsn->null_count(), 0);
  fs::remove_all(path.parent_path());
#endif
}

TEST(Parquet, SameTableTwiceGivesSameBytes) {
  auto writer = make_parquet_writer();
  if (!writer) GTEST_SKIP() << "built without Arrow";
  const auto tables = build_tables(inputs());
  const fs::path a = scratch("a.parquet"), b = scratch("b.parquet");
  ASSERT_TRUE(writer->write(tables.analyses, a));
  ASSERT_TRUE(writer->write(tables.analyses, b));
  EXPECT_EQ(slurp(a), slurp(b));
  EXPECT_GT(slurp(a).size(), 100u);
  fs::remove_all(a.parent_path());
  fs::remove_all(b.parent_path());
}

TEST(Parquet, WriterNameSaysWhatTheBuildHas) {
  if (make_parquet_writer()) {
    EXPECT_TRUE(parquet_writer_name().starts_with("arrow ")) << parquet_writer_name();
  } else {
    EXPECT_TRUE(parquet_writer_name().empty());
  }
}
