// Without Apache Arrow there is no Parquet writer: a build writes everything
// but data/*.parquet and says so (PYCHRON_PUBLISH, libs/publish/CMakeLists.txt).

#include "pychron/publish/parquet.hpp"

namespace pychron::publish {

std::unique_ptr<IParquetWriter> make_parquet_writer() { return nullptr; }
std::string parquet_writer_name() { return {}; }

}  // namespace pychron::publish
