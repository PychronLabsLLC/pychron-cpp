#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "pychron/experiment/model/queue_file.hpp"
#include "pychron/experiment/model/queue_toml.hpp"

using namespace pychron::experiment;

namespace {
const IdentifierRules kIds = IdentifierRules::defaults();

QueueSpec sample_queue() {
  QueueSpec q;
  q.name = "round \"trip\"";
  q.mass_spectrometer = "jan";
  q.extract_device = "FusionsDiode";
  q.tray = "24-well";
  q.load = "L1";
  q.username = "jr";
  q.email = "jr@example.org";
  q.queue_conditionals = "queue_cond";
  q.repository = "repo";
  q.delays.before_analyses = Duration(30);
  q.delays.between_analyses = Duration(12.5);
  q.delays.extract_delay = Duration(0.1);

  RunSpec blank;
  blank.id = {"bu", std::nullopt, "", AnalysisType::BlankUnknown};
  blank.extraction.device = "FusionsDiode";
  blank.measurement.plan = "argon";
  q.runs.push_back(blank);

  RunSpec u;
  u.id = {"20001", 3, "A", AnalysisType::Unknown};
  u.extraction.device = "FusionsDiode";
  u.extraction.position = Position{{1, 2, 3, 7}};
  u.extraction.value = 5.5;
  u.extraction.units = Unit::Percent;
  u.extraction.duration = Duration(30);
  u.extraction.cleanup = Duration(120);
  u.extraction.pre_cleanup = Duration(1);
  u.extraction.post_cleanup = Duration(2);
  u.extraction.pattern = "spiral";
  u.extraction.beam_diameter = 1.25;
  u.extraction.ramp_rate = 0.1;
  u.extraction.ramp = Duration(15);
  u.extraction.cryo_temp = -80;
  u.extraction.script = "felix_co2";
  u.extraction.options = "a=1";
  u.measurement.plan = "argon";
  u.measurement.hook = "my_hook";
  u.measurement.overrides = {{"counts", std::int64_t{20}},
                             {"peak_center", true},
                             {"integration", 1.0},
                             {"detector", std::string("H1")},
                             {"odd key", std::int64_t{1}}};
  u.post_equilibration = "pe";
  u.post_measurement = "pm";
  u.overlap = {Duration(10), Duration(5)};
  u.delay_after = Duration(7);
  u.conditionals = {{"c1", "action"}, {"t1", "truncate"}, {"x", "cancel"}};
  u.comment = "line1\nline2";
  u.weight = 0.003;
  u.skip = true;
  u.end_after = true;
  u.sample = {"NM-1", "sanidine", "P", "NM-300", "A", 4};
  q.runs.push_back(u);

  RunSpec p;
  p.id = {"pa", std::nullopt, "", AnalysisType::Pause};
  q.runs.push_back(p);  // no device although queue has one
  return q;
}
}  // namespace

TEST(QueueFile, DumpParseRoundTrip) {
  const QueueSpec q = sample_queue();
  const std::string text = dump_queue(q);
  auto back = parse_queue(text, kIds);
  ASSERT_TRUE(back) << back.error().what << "\n" << text;
  EXPECT_EQ(*back, q) << text;
  // Dump is deterministic and stable across a second round trip.
  EXPECT_EQ(dump_queue(*back), text);
}

TEST(QueueFile, DumpIsDiffFriendly) {
  const std::string text = dump_queue(sample_queue());
  EXPECT_NE(text.find("schema_version = 1"), std::string::npos);
  EXPECT_NE(text.find("[[runs]]\nidentifier = \"bu\""), std::string::npos) << text;
  EXPECT_NE(text.find("skip = true"), std::string::npos);
  EXPECT_NE(text.find("extract_delay = 0.1\n"), std::string::npos);  // shortest round-trip float
  EXPECT_NE(text.find("before_analyses = 30.0\n"), std::string::npos);
  EXPECT_NE(text.find("comment = \"line1\\nline2\""), std::string::npos);
  EXPECT_NE(text.find("position = \"1-3,7\""), std::string::npos);
  // Blank inherits the queue device, so it is not repeated per run.
  const auto first = text.find("[[runs]]");
  const auto second = text.find("[[runs]]", first + 1);
  EXPECT_EQ(text.substr(first, second - first).find("device"), std::string::npos);
}

TEST(QueueFile, ParseOfDumpNormalizesAliases) {
  auto q = parse_queue("[queue]\nmass_spectrometer=\"j\"\n[[runs]]\nidentifier=\"1\"\ne_value=2\nt_o=\"tr\"\n", kIds);
  ASSERT_TRUE(q);
  const std::string text = dump_queue(*q);
  EXPECT_EQ(text.find("e_value"), std::string::npos);
  EXPECT_EQ(text.find("t_o"), std::string::npos);
  auto back = parse_queue(text, kIds);
  ASSERT_TRUE(back) << back.error().what;
  EXPECT_EQ(*back, *q);
}

TEST(QueueFile, ConditionalTablesParse) {
  auto q = parse_queue(
      "[queue]\n[[runs]]\nidentifier=\"1\"\nconditionals=[\"a\", {name=\"b\", kind=\"cancel\"}]\n", kIds);
  ASSERT_TRUE(q) << q.error().what;
  ASSERT_EQ(q->runs[0].conditionals.size(), 2u);
  EXPECT_EQ(q->runs[0].conditionals[1], (ConditionalRef{"b", "cancel"}));
  EXPECT_FALSE(parse_queue("[queue]\n[[runs]]\nidentifier=\"1\"\nconditionals=[{kind=\"cancel\"}]\n", kIds));
  EXPECT_FALSE(parse_queue("[queue]\n[[runs]]\nidentifier=\"1\"\nconditionals=[{name=\"b\", bogus=1}]\n", kIds));
}

TEST(QueueFile, SaveAndLoadFile) {
  const auto dir = std::filesystem::temp_directory_path() / "pychron_queue_file_test";
  std::filesystem::create_directories(dir);
  const auto path = (dir / "experiment.toml").string();
  const QueueSpec q = sample_queue();
  ASSERT_TRUE(save_queue_file(path, q));
  auto back = load_queue_file(path, kIds);
  ASSERT_TRUE(back) << back.error().what;
  EXPECT_EQ(*back, q);
  std::filesystem::remove_all(dir);
}

TEST(QueueFile, LoadMissingFileIsIoError) {
  auto r = load_queue_file("/nonexistent/dir/experiment.toml", kIds);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, pychron::ErrorKind::Io);
}
