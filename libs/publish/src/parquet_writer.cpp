// Writing a release table as (Geo)Parquet with Apache Arrow (data publishing
// design, section 5.2). The only translation unit that sees Arrow; without
// PYCHRON_PUBLISH_ARROW_ENABLED it is empty and parquet_stub.cpp stands in.

#include "pychron/publish/parquet.hpp"

#ifdef PYCHRON_PUBLISH_ARROW_ENABLED

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <arrow/util/config.h>
#include <arrow/util/key_value_metadata.h>
#include <parquet/arrow/writer.h>
#include <parquet/properties.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pychron::publish {

namespace {

// PROJJSON of OGC:CRS84 (WGS 84 longitude, latitude), as the GeoParquet
// specification's example gives it.
constexpr const char* kCrs84Projjson = R"js({"$schema":"https://proj.org/schemas/v0.7/projjson.schema.json",)js"
    R"js("type":"GeographicCRS","name":"WGS 84 (CRS84)",)js"
    R"js("datum_ensemble":{"name":"World Geodetic System 1984 ensemble","members":[)js"
    R"js({"name":"World Geodetic System 1984 (Transit)","id":{"authority":"EPSG","code":1166}},)js"
    R"js({"name":"World Geodetic System 1984 (G730)","id":{"authority":"EPSG","code":1152}},)js"
    R"js({"name":"World Geodetic System 1984 (G873)","id":{"authority":"EPSG","code":1153}},)js"
    R"js({"name":"World Geodetic System 1984 (G1150)","id":{"authority":"EPSG","code":1154}},)js"
    R"js({"name":"World Geodetic System 1984 (G1674)","id":{"authority":"EPSG","code":1155}},)js"
    R"js({"name":"World Geodetic System 1984 (G1762)","id":{"authority":"EPSG","code":1156}},)js"
    R"js({"name":"World Geodetic System 1984 (G2139)","id":{"authority":"EPSG","code":1309}}],)js"
    R"js("ellipsoid":{"name":"WGS 84","semi_major_axis":6378137,"inverse_flattening":298.257223563},)js"
    R"js("accuracy":"2.0","id":{"authority":"EPSG","code":6326}},)js"
    R"js("coordinate_system":{"subtype":"ellipsoidal","axis":[)js"
    R"js({"name":"Geodetic longitude","abbreviation":"Lon","direction":"east","unit":"degree"},)js"
    R"js({"name":"Geodetic latitude","abbreviation":"Lat","direction":"north","unit":"degree"}]},)js"
    R"js("scope":"Not known.","area":"World.",)js"
    R"js("bbox":{"south_latitude":-90,"west_longitude":-180,"north_latitude":90,"east_longitude":180},)js"
    R"js("id":{"authority":"OGC","code":"CRS84"}})js";

Result<void> status_of(const arrow::Status& st, const char* what) {
  if (st.ok()) return {};
  return fail(ErrorKind::Io, std::string("parquet: ") + what + ": " + st.ToString());
}

std::shared_ptr<arrow::DataType> arrow_type(ColumnType type) {
  switch (type) {
    case ColumnType::Double:
      return arrow::float64();
    case ColumnType::Int32:
      return arrow::int32();
    case ColumnType::Int64:
      return arrow::int64();
    case ColumnType::Bool:
      return arrow::boolean();
    case ColumnType::String:
    case ColumnType::Json:
      return arrow::utf8();
    case ColumnType::TimestampUtc:
      return arrow::timestamp(arrow::TimeUnit::MICRO, "UTC");
    case ColumnType::Wkb:
      return arrow::binary();
  }
  return arrow::utf8();
}

std::shared_ptr<arrow::DataType> bbox_type() {
  return arrow::struct_({arrow::field("xmin", arrow::float64()), arrow::field("ymin", arrow::float64()),
                         arrow::field("xmax", arrow::float64()), arrow::field("ymax", arrow::float64())});
}

struct Bbox {
  double xmin, ymin, xmax, ymax;
};

std::optional<Bbox> bbox_of(const Cell& c) {
  const auto* j = std::get_if<JsonText>(&c);
  if (!j) return std::nullopt;
  const auto parsed = nlohmann::json::parse(j->value, nullptr, false);
  if (!parsed.is_object() || !parsed.contains("xmin")) return std::nullopt;
  return Bbox{parsed["xmin"].get<double>(), parsed["ymin"].get<double>(), parsed["xmax"].get<double>(),
              parsed["ymax"].get<double>()};
}

// One column's array from its cells.
Result<std::shared_ptr<arrow::Array>> array_of(const Table& t, std::size_t col) {
  const Column& column = t.columns[col];
  arrow::MemoryPool* pool = arrow::default_memory_pool();
  const auto rows = static_cast<int64_t>(t.rows.size());
  std::shared_ptr<arrow::Array> out;
  arrow::Status st;
  const bool is_bbox = t.geo && column.name == "bbox" && column.type == ColumnType::Json;
  if (is_bbox) {
    auto xmin = std::make_shared<arrow::DoubleBuilder>(pool), ymin = std::make_shared<arrow::DoubleBuilder>(pool),
         xmax = std::make_shared<arrow::DoubleBuilder>(pool), ymax = std::make_shared<arrow::DoubleBuilder>(pool);
    arrow::StructBuilder builder(bbox_type(), pool, {xmin, ymin, xmax, ymax});
    for (const auto& row : t.rows) {
      if (auto b = bbox_of(row[col])) {
        st = builder.Append();
        if (st.ok()) st = xmin->Append(b->xmin);
        if (st.ok()) st = ymin->Append(b->ymin);
        if (st.ok()) st = xmax->Append(b->xmax);
        if (st.ok()) st = ymax->Append(b->ymax);
      } else {
        st = builder.AppendNull();
        if (st.ok()) st = xmin->AppendNull();
        if (st.ok()) st = ymin->AppendNull();
        if (st.ok()) st = xmax->AppendNull();
        if (st.ok()) st = ymax->AppendNull();
      }
      if (!st.ok()) return fail(ErrorKind::Io, "parquet: bbox: " + st.ToString());
    }
    st = builder.Finish(&out);
    if (!st.ok()) return fail(ErrorKind::Io, "parquet: bbox: " + st.ToString());
    return out;
  }
  switch (column.type) {
    case ColumnType::Double: {
      arrow::DoubleBuilder b(pool);
      st = b.Reserve(rows);
      for (const auto& row : t.rows) {
        const auto* v = std::get_if<double>(&row[col]);
        st = (v && std::isfinite(*v)) ? b.Append(*v) : b.AppendNull();
        if (!st.ok()) break;
      }
      if (st.ok()) st = b.Finish(&out);
      break;
    }
    case ColumnType::Int32: {
      arrow::Int32Builder b(pool);
      for (const auto& row : t.rows) {
        const auto* v = std::get_if<std::int32_t>(&row[col]);
        st = v ? b.Append(*v) : b.AppendNull();
        if (!st.ok()) break;
      }
      if (st.ok()) st = b.Finish(&out);
      break;
    }
    case ColumnType::Int64: {
      arrow::Int64Builder b(pool);
      for (const auto& row : t.rows) {
        const auto* v = std::get_if<std::int64_t>(&row[col]);
        st = v ? b.Append(*v) : b.AppendNull();
        if (!st.ok()) break;
      }
      if (st.ok()) st = b.Finish(&out);
      break;
    }
    case ColumnType::Bool: {
      arrow::BooleanBuilder b(pool);
      for (const auto& row : t.rows) {
        const auto* v = std::get_if<bool>(&row[col]);
        st = v ? b.Append(*v) : b.AppendNull();
        if (!st.ok()) break;
      }
      if (st.ok()) st = b.Finish(&out);
      break;
    }
    case ColumnType::String:
    case ColumnType::Json: {
      arrow::StringBuilder b(pool);
      for (const auto& row : t.rows) {
        if (const auto* s = std::get_if<std::string>(&row[col])) st = b.Append(*s);
        else if (const auto* j = std::get_if<JsonText>(&row[col])) st = b.Append(j->value);
        else st = b.AppendNull();
        if (!st.ok()) break;
      }
      if (st.ok()) st = b.Finish(&out);
      break;
    }
    case ColumnType::TimestampUtc: {
      arrow::TimestampBuilder b(arrow::timestamp(arrow::TimeUnit::MICRO, "UTC"), pool);
      for (const auto& row : t.rows) {
        const auto* v = std::get_if<UtcTime>(&row[col]);
        st = v ? b.Append(v->micros) : b.AppendNull();
        if (!st.ok()) break;
      }
      if (st.ok()) st = b.Finish(&out);
      break;
    }
    case ColumnType::Wkb: {
      arrow::BinaryBuilder b(pool);
      for (const auto& row : t.rows) {
        const auto* v = std::get_if<WkbBytes>(&row[col]);
        st = v ? b.Append(v->value) : b.AppendNull();
        if (!st.ok()) break;
      }
      if (st.ok()) st = b.Finish(&out);
      break;
    }
  }
  if (!st.ok()) return fail(ErrorKind::Io, "parquet: " + column.name + ": " + st.ToString());
  return out;
}

// The `geo` file metadata of a table with a geometry column (GeoParquet 1.1.0).
std::string geo_metadata(const Table& t) {
  double w = 180, s = 90, e = -180, n = -90;
  bool any = false;
  const auto bbox_col = t.column("bbox");
  if (bbox_col)
    for (const auto& row : t.rows)
      if (auto b = bbox_of(row[*bbox_col])) {
        any = true;
        w = std::min(w, b->xmin);
        s = std::min(s, b->ymin);
        e = std::max(e, b->xmax);
        n = std::max(n, b->ymax);
      }
  nlohmann::ordered_json geometry;
  geometry["encoding"] = "WKB";
  geometry["geometry_types"] = nlohmann::json::array({"Point"});
  geometry["crs"] = nlohmann::json::parse(kCrs84Projjson);
  geometry["orientation"] = "counterclockwise";
  geometry["edges"] = "planar";
  if (any) geometry["bbox"] = nlohmann::json::array({w, s, e, n});
  nlohmann::ordered_json covering;
  covering["bbox"] = {{"xmin", {"bbox", "xmin"}}, {"ymin", {"bbox", "ymin"}}, {"xmax", {"bbox", "xmax"}},
                      {"ymax", {"bbox", "ymax"}}};
  geometry["covering"] = covering;
  nlohmann::ordered_json geo;
  geo["version"] = "1.1.0";
  geo["primary_column"] = "geometry";
  geo["columns"] = {{"geometry", geometry}};
  return geo.dump();
}

class ArrowWriter final : public IParquetWriter {
 public:
  Result<void> write(const Table& t, const std::filesystem::path& path) override {
    std::vector<std::shared_ptr<arrow::Field>> fields;
    std::vector<std::shared_ptr<arrow::Array>> arrays;
    for (std::size_t i = 0; i < t.columns.size(); ++i) {
      const Column& c = t.columns[i];
      const bool is_bbox = t.geo && c.name == "bbox" && c.type == ColumnType::Json;
      fields.push_back(arrow::field(c.name, is_bbox ? bbox_type() : arrow_type(c.type), c.nullable));
      auto array = array_of(t, i);
      if (!array) return fail(array.error());
      arrays.push_back(*array);
    }
    auto schema = arrow::schema(fields);
    if (t.geo) {
      auto metadata = std::make_shared<arrow::KeyValueMetadata>();
      metadata->Append("geo", geo_metadata(t));
      schema = schema->WithMetadata(metadata);
    }
    const auto table = arrow::Table::Make(schema, arrays, static_cast<int64_t>(t.rows.size()));
    auto outfile = arrow::io::FileOutputStream::Open(path.string());
    if (!outfile.ok()) return fail(ErrorKind::Io, "parquet: cannot write " + path.string() + ": " + outfile.status().ToString());
    parquet::WriterProperties::Builder builder;
    builder.compression(parquet::Compression::ZSTD);
    builder.max_row_group_length(std::max<int64_t>(1, static_cast<int64_t>(t.rows.size())));
    builder.created_by("pychron-cpp parquet writer");
    const auto props = builder.build();
    const auto arrow_props = parquet::ArrowWriterProperties::Builder().store_schema()->build();
    const auto st = parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), *outfile,
                                               std::max<int64_t>(1, static_cast<int64_t>(t.rows.size())), props,
                                               arrow_props);
    if (auto r = status_of(st, "write"); !r) return r;
    return status_of((*outfile)->Close(), "close");
  }
};

}  // namespace

std::unique_ptr<IParquetWriter> make_parquet_writer() { return std::make_unique<ArrowWriter>(); }
std::string_view parquet_writer_name() {
  static const std::string name = std::string("arrow ") + ARROW_VERSION_STRING;
  return name;
}

}  // namespace pychron::publish

#endif  // PYCHRON_PUBLISH_ARROW_ENABLED
