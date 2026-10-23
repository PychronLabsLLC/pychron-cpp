// The Ar-Ar extension loader of a records directory (domain seam spec 3.3):
// references.toml beside the records gives an analysis the flux of its
// identifier and the production and chronology of its irradiation, and a
// changed file is read again on the next refresh.

#include "pychron/arar/record_loader.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

#include "pychron/arar/analysis_data.hpp"
#include "pychron/arar/reduce.hpp"
#include "pychron/processing/quantity.hpp"
#include "pychron/processing/record_source.hpp"
#include "record_fixtures.hpp"

namespace {

namespace pp = pychron::processing;
namespace fs = std::filesystem;
using pp::test::record;
using pp::test::TempDir;
using pp::test::write;

constexpr const char* kReferences = R"(
[flux."66001"]
j = 0.001
j_err = 1e-6

[production."NM-300"]
K4039 = [0.0002, 0.00001]
Ca3937 = [0.0007, 0.00001]

[[chronology."NM-300"]]
power = 1.0
start = "2026-01-01T00:00:00Z"
end = "2026-01-01T10:00:00Z"
)";

TEST(RecordLoader, AppliesReferencesByIdentifierAndIrradiation) {
  TempDir dir;
  write(dir.path, record("u1", "66001", 1, "", "unknown", "2026-09-30T10:00:00Z", 1000.0, 0.5));
  write(dir.path, record("u2", "66002", 1, "", "unknown", "2026-09-30T11:00:00Z", 1000.0, 0.5));
  std::ofstream(dir.path / "references.toml") << kReferences;
  pp::RecordDirectorySource src(dir.path);
  src.set_extension_loader(pychron::arar::record_extension_loader(dir.path / "references.toml"));

  auto a = src.load("u1");
  ASSERT_TRUE(a) << a.error().what;
  const auto* data = pychron::arar::analysis_data_of(**a);
  ASSERT_NE(data, nullptr);
  ASSERT_TRUE(data->flux);
  EXPECT_DOUBLE_EQ(data->flux->j.value, 0.001);
  EXPECT_DOUBLE_EQ(data->flux->j.error, 1e-6);
  ASSERT_TRUE(data->production);
  EXPECT_DOUBLE_EQ(data->production->k4039.value, 0.0002);
  EXPECT_EQ(data->chronology.size(), 1u);
  auto ra = pychron::arar::reduce_with_arar(*a);
  auto age = pp::Quantity::parse("age")->eval(*ra);
  ASSERT_TRUE(age) << ra->reduction_error;
  EXPECT_GT(age->value, 0.0);

  // Another identifier of the same irradiation: its production and
  // chronology, no flux, so no age.
  auto b = src.load("u2");
  ASSERT_TRUE(b) << b.error().what;
  const auto* other = pychron::arar::analysis_data_of(**b);
  ASSERT_NE(other, nullptr);
  EXPECT_FALSE(other->flux);
  EXPECT_TRUE(other->production);
  EXPECT_EQ(other->chronology.size(), 1u);
  EXPECT_FALSE(pp::Quantity::parse("age")->eval(*pychron::arar::reduce_with_arar(*b)));
}

// A changed references.toml is read on the next refresh, which maps every
// record again and moves the generation; one that is gone takes the data away.
TEST(RecordLoader, AChangedReferencesFileIsReadOnRefresh) {
  TempDir dir;
  write(dir.path, record("u1", "66001", 1, "", "unknown", "2026-09-30T10:00:00Z", 1000.0, 0.5));
  const fs::path file = dir.path / "references.toml";
  std::ofstream(file) << kReferences;
  pp::RecordDirectorySource src(dir.path);
  src.set_extension_loader(pychron::arar::record_extension_loader(file));
  ASSERT_TRUE(src.refresh());
  const auto g0 = src.generation();
  ASSERT_TRUE(src.refresh());
  EXPECT_EQ(src.generation(), g0);  // nothing changed

  {
    std::ofstream out(file);
    out << "[flux.\"66001\"]\nj = 0.002\nj_err = 2e-6\n";
  }
  fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(5));
  ASSERT_TRUE(src.refresh());
  EXPECT_GT(src.generation(), g0);
  auto a = src.load("u1");
  ASSERT_TRUE(a) << a.error().what;
  const auto* data = pychron::arar::analysis_data_of(**a);
  ASSERT_NE(data, nullptr);
  ASSERT_TRUE(data->flux);
  EXPECT_DOUBLE_EQ(data->flux->j.value, 0.002);
  EXPECT_FALSE(data->production);

  const auto g1 = src.generation();
  fs::remove(file);
  ASSERT_TRUE(src.refresh());
  EXPECT_GT(src.generation(), g1);
  a = src.load("u1");
  ASSERT_TRUE(a);
  ASSERT_NE(pychron::arar::analysis_data_of(**a), nullptr);
  EXPECT_FALSE(pychron::arar::analysis_data_of(**a)->flux);

  // A file that does not parse fails the refresh.
  std::ofstream(file) << "[flux.\"66001\"\nj = ";
  EXPECT_FALSE(src.refresh());
}

}  // namespace
