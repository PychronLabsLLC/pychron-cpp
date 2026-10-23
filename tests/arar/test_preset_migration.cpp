// A pipeline saved before the domain seam split the reduction (spec 4,
// "Presets") carries the Ar-Ar reduction's options on its reduce node. Read
// over the Ar-Ar registry, they move to the arar_reduce node that follows it,
// once, with a warning naming each key, and are written back there.

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "arar_fixtures.hpp"
#include "pychron/arar/domain.hpp"
#include "pychron/processing/domain.hpp"
#include "pychron/processing/units.hpp"

namespace pp = pychron::processing;

namespace {

// select, reduce, arar_reduce as a file written before the split: the reduce
// node holds constants and include_decay_error.
constexpr const char* kPreSplit = R"(schema = "pipeline"
version = 1
name = "Pre-split"

[[units]]
id = "select"
kind = "select"
inputs = []

[[units]]
id = "reduce"
kind = "reduce"
inputs = ["select"]

[units.options]
constants = "legacy"
include_decay_error = true

[[units]]
id = "arar_reduce"
kind = "arar_reduce"
inputs = ["reduce"]
)";

std::size_t count_naming(const std::vector<std::string>& warnings, const std::string& key) {
  return static_cast<std::size_t>(
      std::count_if(warnings.begin(), warnings.end(), [&](const std::string& w) { return w.find(key) != std::string::npos; }));
}

}  // namespace

TEST(Pipeline, ReducePresetKeysMigrateToArArReduce) {
  const auto domain = pychron::arar::make_domain();
  std::vector<std::string> warnings;
  auto p = pp::pipeline_from_toml(domain->units(), kPreSplit, &warnings);
  ASSERT_TRUE(p) << p.error().what;

  // One warning per moved key, naming it.
  ASSERT_EQ(warnings.size(), 2u);
  EXPECT_EQ(count_naming(warnings, "constants"), 1u);
  EXPECT_NE(std::find(warnings.begin(), warnings.end(), "'reduce': option 'constants' now belongs to arar_reduce"),
            warnings.end());
  EXPECT_NE(
      std::find(warnings.begin(), warnings.end(), "'reduce': option 'include_decay_error' now belongs to arar_reduce"),
      warnings.end());

  const auto* reduce = p->find("reduce");
  const auto* arar = p->find("arar_reduce");
  ASSERT_NE(reduce, nullptr);
  ASSERT_NE(arar, nullptr);
  EXPECT_TRUE(reduce->options.extra.empty());
  EXPECT_EQ(arar->options.get_string("constants"), "legacy");
  EXPECT_TRUE(arar->options.get_bool("include_decay_error"));

  // Written back under the new node: read again, nothing moves.
  const std::string saved = pp::pipeline_to_toml(*p);
  std::vector<std::string> again;
  auto back = pp::pipeline_from_toml(domain->units(), saved, &again);
  ASSERT_TRUE(back) << back.error().what << "\n" << saved;
  EXPECT_TRUE(again.empty()) << again.front();
  EXPECT_EQ(back->find("arar_reduce")->options.get_string("constants"), "legacy");
  EXPECT_TRUE(back->find("reduce")->options.extra.empty());

  // And it runs with them.
  pp::MemorySource source;
  source.add(pychron::arar::test::make_unknown(1));
  pp::Runner runner(domain->units(), &source, domain.get());
  auto ran = runner.run(*p, "arar_reduce");
  ASSERT_TRUE(ran) << ran.error().what;
  EXPECT_EQ(std::get<pp::DatasetPtr>(ran->at(0))->reduction_tag, "arar-1/legacy/d1e0");
}

// A value the new node's schema refuses is not moved: the default stands and
// the warning says why.
TEST(Pipeline, AMigratedKeyIsValidatedByItsNewNode) {
  const auto domain = pychron::arar::make_domain();
  std::string text = kPreSplit;
  text.replace(text.find("\"legacy\""), 8, "\"bogus\"");
  std::vector<std::string> warnings;
  auto p = pp::pipeline_from_toml(domain->units(), text, &warnings);
  ASSERT_TRUE(p) << p.error().what;
  EXPECT_EQ(count_naming(warnings, "constants"), 1u);
  EXPECT_EQ(p->find("arar_reduce")->options.get_string("constants"), "default");
  EXPECT_FALSE(p->find("arar_reduce")->options.is_set("constants"));
  EXPECT_TRUE(p->find("reduce")->options.extra.empty());
}

// With no arar_reduce node after reduce, nothing moves: the key stays where
// it was read, kept unread and written back unchanged, as any unknown key.
TEST(Pipeline, ReduceKeysStayWithoutAnArArReduceNode) {
  const auto domain = pychron::arar::make_domain();
  std::string text = kPreSplit;
  text.erase(text.find("[[units]]\nid = \"arar_reduce\""));
  std::vector<std::string> warnings;
  auto p = pp::pipeline_from_toml(domain->units(), text, &warnings);
  ASSERT_TRUE(p) << p.error().what;
  EXPECT_TRUE(warnings.empty()) << warnings.front();
  EXPECT_EQ(p->find("reduce")->options.extra.count("constants"), 1u);
}

// A value the new node sets itself wins over the stale copy, which is still
// taken off the old node.
TEST(Pipeline, AnArArReduceValueWinsOverAMigratedOne) {
  const auto domain = pychron::arar::make_domain();
  const std::string text = std::string(kPreSplit) + "\n[units.options]\nconstants = \"legacy_preferences\"\n";
  std::vector<std::string> warnings;
  auto p = pp::pipeline_from_toml(domain->units(), text, &warnings);
  ASSERT_TRUE(p) << p.error().what;
  EXPECT_EQ(count_naming(warnings, "constants"), 1u);
  EXPECT_EQ(p->find("arar_reduce")->options.get_string("constants"), "legacy_preferences");
  EXPECT_TRUE(p->find("arar_reduce")->options.get_bool("include_decay_error"));
  EXPECT_TRUE(p->find("reduce")->options.extra.empty());
}
