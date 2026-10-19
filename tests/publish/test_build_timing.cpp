// How long creating and building a release takes against the number of
// analyses (data publishing design, section 6.1; libs/persistence/AGENTS.md
// "How to know"). Does nothing unless told how many analyses to make:
//
//   PYCHRON_BENCH_N=400 build/dev/tests/publish/pychron_publish_tests --gtest_filter='BuildTiming.*'
//
// Ingests N unknowns of five isotopes on one irradiated level into a temp
// SQLite store, creates a release of them and builds it, printing the time
// of each step. Time that grows faster than N is a query per analysis.

#include <gtest/gtest.h>

#include <charconv>
#include <chrono>
#include <cstdlib>
#include <string_view>
#include <filesystem>
#include <iostream>
#include <random>

#include "pychron/processing/store_source.hpp"
#include "pychron/publish/build.hpp"
#include "pychron/publish/release.hpp"
#include "release_fixture.hpp"

namespace fs = std::filesystem;
namespace ps = pychron::persistence;
namespace pp = pychron::processing;
using namespace pychron;
using namespace pychron::publish;

namespace {

class BuildTiming : public pychron::publish::testing::ReleaseFixture {};

double seconds_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

TEST_P(BuildTiming, CreateAndBuildScaleWithTheCount) {
  const char* n_text = std::getenv("PYCHRON_BENCH_N");
  if (!n_text) GTEST_SKIP() << "set PYCHRON_BENCH_N";
  if (GetParam() != "sqlite") GTEST_SKIP();
  int n = 0;
  const std::string_view n_view(n_text);
  if (std::from_chars(n_view.data(), n_view.data() + n_view.size(), n).ec != std::errc{} || n <= 0)
    GTEST_SKIP() << "PYCHRON_BENCH_N is not a count";
  auto t0 = std::chrono::steady_clock::now();
  std::vector<std::string> uuids;
  uuids.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    // Every analysis on the irradiated identifier 70001: positions, flux and
    // production resolve for each (the per-analysis reads the loader does).
    uuids.push_back(ingest("70001", 10 + i, "2026-10-02T10:00:00Z").str());
  }
  std::cout << "ingest " << n << ": " << seconds_since(t0) << " s\n";

  ReleaseSpec spec;
  spec.name = "bench";
  spec.title = "bench";
  spec.analyses = uuids;
  spec.options_json = release_options_json(ReleaseOptions{});
  spec.reduction_version = "arar-1";
  spec.software_version = "pychron-cpp/bench";
  t0 = std::chrono::steady_clock::now();
  auto created = create_release(*store_, reducer(), spec);
  ASSERT_TRUE(created) << to_string(created.error());
  std::cout << "create " << n << ": " << seconds_since(t0) << " s\n";

  auto source = pp::StoreSource::open(ps::StoreConfig{db_->url(), false}, pp::StoreSourceOptions{2, "bench", "bench"});
  ASSERT_TRUE(source);
  const fs::path out = fs::temp_directory_path() / ("pychron_bench_" + std::to_string(std::random_device{}()));
  fs::create_directories(out);
  BuildOptions options;
  options.out = out;
  options.store_url = db_->url();
  t0 = std::chrono::steady_clock::now();
  auto built = build_release(*store_, **source, created->release, options, nullptr);
  ASSERT_TRUE(built) << to_string(built.error());
  std::cout << "build " << n << ": " << seconds_since(t0) << " s (" << built->manifest.files.size() << " files"
            << (built->parquet_written ? ", with Parquet" : "") << ")\n";
  source->reset();
  fs::remove_all(out);
}

INSTANTIATE_TEST_SUITE_P(Engines, BuildTiming, ::testing::ValuesIn(ps::testing::engines()),
                         [](const ::testing::TestParamInfo<std::string>& i) { return i.param; });
