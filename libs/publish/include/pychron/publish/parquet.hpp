#pragma once

// Writing a release table as (Geo)Parquet (data publishing design, section
// 5.2). Apache Arrow is optional (PYCHRON_PUBLISH): without it
// make_parquet_writer() is null and a build writes everything but
// data/*.parquet and says so.

#include <filesystem>
#include <memory>
#include <string_view>

#include "pychron/core/error.hpp"
#include "pychron/publish/tables.hpp"

namespace pychron::publish {

class IParquetWriter {
 public:
  virtual ~IParquetWriter() = default;
  // One row group, ZSTD, timestamps in microseconds UTC; a `geo` table gets
  // GeoParquet 1.1 metadata (WKB, OGC:CRS84, a bbox covering) and a bbox
  // struct column. Same table twice gives the same bytes.
  virtual Result<void> write(const Table& table, const std::filesystem::path& path) = 0;
};

std::unique_ptr<IParquetWriter> make_parquet_writer();
// What the build was compiled with: "arrow <version>" or "" without Arrow.
std::string_view parquet_writer_name();

}  // namespace pychron::publish
