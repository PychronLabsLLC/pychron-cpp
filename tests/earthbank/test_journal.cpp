#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "pychron/core/error.hpp"
#include "pychron/earthbank/journal.hpp"
#include "upload_support.hpp"

namespace pychron::earthbank {
namespace {

namespace fs = std::filesystem;
using testing::TempDir;

Journal sample_journal() {
  Journal j;
  j.hash = "9f2c4d";
  j.sample_id = 710001;
  j.data_point_id = 740001;
  j.age_calc_id = 770001;
  j.aliquots = {{"20001-01", 750001}, {"20002-01", 750002}};
  j.measurements = {{"0001", 760001}, {"0002", 760002}};
  j.in_doubt = "age summary";
  return j;
}

std::string slurp(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void spit(const fs::path& file, std::string_view text) {
  fs::create_directories(file.parent_path());
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out << text;
}

TEST(Journal, RoundTripAndAtomicSave) {
  TempDir dir;
  const fs::path file = journal_file(dir.path(), "earthbank.test", 12345, "S-1");
  const auto none = load_journal(file);
  ASSERT_TRUE(none.has_value());
  EXPECT_FALSE(none->has_value()) << "no file is no journal";

  const Journal first = sample_journal();
  ASSERT_TRUE(save_journal(first, file).has_value());
  auto back = load_journal(file);
  ASSERT_TRUE(back.has_value());
  ASSERT_TRUE(back->has_value());
  EXPECT_EQ(**back, first);

  // Replaced whole: the next save is the next journal, and nothing is left
  // beside the file.
  Journal second = first;
  second.in_doubt.reset();
  second.age_summary_id = 780001;
  ASSERT_TRUE(save_journal(second, file).has_value());
  back = load_journal(file);
  ASSERT_TRUE(back.has_value() && back->has_value());
  EXPECT_EQ(**back, second);
  std::set<std::string> names;
  for (const auto& entry : fs::directory_iterator(file.parent_path())) names.insert(entry.path().filename().string());
  EXPECT_EQ(names, std::set<std::string>{"%53-1.json"});
}

TEST(Journal, TheFileIsTheDocumentedJson) {
  TempDir dir;
  const fs::path file = dir.path() / "j.json";
  ASSERT_TRUE(save_journal(sample_journal(), file).has_value());
  const auto doc = nlohmann::json::parse(slurp(file));
  EXPECT_EQ(doc["version"], 1);
  EXPECT_EQ(doc["hash"], "9f2c4d");
  EXPECT_EQ(doc["sampleId"], 710001);
  EXPECT_EQ(doc["arArDataPointId"], 740001);
  EXPECT_EQ(doc["aliquots"]["20001-01"], 750001);
  EXPECT_EQ(doc["measurements"]["0002"], 760002);
  EXPECT_EQ(doc["ageCalcId"], 770001);
  EXPECT_EQ(doc["inDoubt"], "age summary");
  EXPECT_FALSE(doc.contains("ageSummaryId"));
}

TEST(Journal, FileNameEncodesTheSample) {
  const fs::path dir = "journals";
  const auto name = [&](std::string_view sample) { return journal_file(dir, "earthbank.test", 7, sample).filename().string(); };
  EXPECT_EQ(name("s-1_a.b"), "s-1_a.b.json");
  EXPECT_EQ(name("S-1"), "%53-1.json") << "upper case is escaped";
  EXPECT_EQ(name("A/B"), "%41%2F%42.json");
  EXPECT_EQ(name("Caf\xC3\xA9"), "%43af%C3%A9.json");
  EXPECT_EQ(name(".."), "%2E..json");
  EXPECT_EQ(name("."), "%2E.json");
  EXPECT_EQ(name("a b&c<d>\"e'f%g+h\\i:j"), "a%20b%26c%3Cd%3E%22e%27f%25g%2Bh%5Ci%3Aj.json");
  EXPECT_EQ(name(""), "%.json");

  // Where it goes: <dir>/<host>/<package>/<sample>.json, and never outside.
  EXPECT_EQ(journal_file(dir, "127.0.0.1:8080", 7, "S").generic_string(), "journals/127.0.0.1%3A8080/7/%53.json");
  for (const std::string_view hostile : {"../../x", "/etc/passwd", "..", "a/../../b", "C:\\x"}) {
    const fs::path file = journal_file(dir, hostile, 7, hostile);
    EXPECT_EQ(file.lexically_normal().lexically_relative(dir).begin()->string() != "..", true) << file;
    EXPECT_EQ(std::distance(file.begin(), file.end()), std::distance(dir.begin(), dir.end()) + 3) << file;
  }

  // Different names are different files, and a name that looks encoded is not
  // taken for the name it would encode.
  EXPECT_NE(name("A/B"), name("A%2FB"));
}

TEST(Journal, NamesThatDifferOnlyByCaseAreDifferentFilesEvenWhereCaseIsIgnored) {
  const fs::path dir = "journals";
  const auto folded = [&](std::string_view sample) {
    std::string name = journal_file(dir, "h", 1, sample).filename().string();
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return name;
  };
  EXPECT_NE(folded("abc"), folded("ABC"));
  EXPECT_NE(folded("Abc"), folded("aBc"));
  // Outside ASCII a name is escaped byte by byte, so the file name is ASCII and neither case folding nor
  // a normalisation of Unicode by the file system can join e-acute and E-acute, or NFC and NFD.
  EXPECT_NE(folded("\xC3\xA9"), folded("\xC3\x89"));
  EXPECT_NE(folded("\xC3\xA9"), folded("e\xCC\x81"));
  for (const unsigned char c : journal_file(dir, "h", 1, "\xC3\x89 e\xCC\x81").filename().string()) EXPECT_LT(c, 0x80);
}

TEST(Journal, ALongNameIsCutAndKeepsItsOwnHash) {
  const fs::path dir = "journals";
  const std::string a(400, 'a');
  const std::string b = a + "b";
  const std::string fa = journal_file(dir, "h", 1, a).filename().string();
  const std::string fb = journal_file(dir, "h", 1, b).filename().string();
  EXPECT_LE(fa.size(), 200u);
  EXPECT_NE(fa, fb) << "two long names that differ only at the end";
  EXPECT_EQ(fa, journal_file(dir, "h", 1, a).filename().string()) << "and the same name is the same file";
  const std::string accents(200, '\xC3');
  EXPECT_LE(journal_file(dir, "h", 1, accents).filename().string().size(), 200u);
}

TEST(Journal, ABrokenFileIsAnErrorAndNeverNoJournal) {
  TempDir dir;
  const fs::path file = dir.path() / "j.json";
  const std::string good = [&] {
    EXPECT_TRUE(save_journal(sample_journal(), file).has_value());
    return slurp(file);
  }();
  const auto expect_refused = [&](std::string_view text, std::string_view why) {
    spit(file, text);
    const auto loaded = load_journal(file);
    ASSERT_FALSE(loaded.has_value()) << why;
    EXPECT_EQ(loaded.error().kind, ErrorKind::Config) << why;
    EXPECT_NE(loaded.error().what.find("j.json"), std::string::npos) << why << ": " << loaded.error().what;
  };
  expect_refused("", "empty");
  expect_refused(good.substr(0, good.size() / 2), "cut short");
  expect_refused("[]", "not an object");
  expect_refused(R"({"hash":"x"})", "no version");
  expect_refused(R"({"version":1})", "no hash");
  expect_refused(R"({"version":2,"hash":"x"})", "another version");
  expect_refused(R"({"version":1,"hash":"x","sampleID":3})", "unknown key");
  expect_refused(R"({"version":1,"hash":"x","sampleId":0})", "id 0");
  expect_refused(R"({"version":1,"hash":"x","sampleId":"7"})", "id as text");
  expect_refused(R"({"version":1,"hash":"x","aliquots":{"a":-1}})", "negative id");
  expect_refused(R"({"version":1,"hash":"x","measurements":[]})", "not an object");
  expect_refused(R"({"version":1,"hash":"x","inDoubt":""})", "empty doubt");
}

TEST(Journal, AFileLeftByACrashedSaveIsIgnored) {
  TempDir dir;
  const fs::path file = dir.path() / "S-1.json";
  ASSERT_TRUE(save_journal(sample_journal(), file).has_value());
  // What a save killed half way leaves: a partial file beside the journal.
  spit(dir.path() / "S-1.json.tmp.99999.0", "{\"version\":1,\"ha");
  const auto loaded = load_journal(file);
  ASSERT_TRUE(loaded.has_value() && loaded->has_value());
  EXPECT_EQ(**loaded, sample_journal());
  // And the next save is not troubled by it.
  Journal next = sample_journal();
  next.in_doubt.reset();
  ASSERT_TRUE(save_journal(next, file).has_value());
}

TEST(Journal, OnlyOneHolderHasTheLock) {
  TempDir dir;
  const fs::path file = journal_file(dir.path(), "earthbank.test", 1, "S-1");
  auto first = lock_journal(file);
  ASSERT_TRUE(first.has_value()) << to_string(first.error());
  EXPECT_TRUE(first->held());
  const auto second = lock_journal(file);
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error().code, std::string(kJournalBusy));
  // Another sample is not in its way.
  EXPECT_TRUE(lock_journal(journal_file(dir.path(), "earthbank.test", 1, "S-2")).has_value());
  // Let go, and it is free: by release, by destruction, by moving over.
  first->release();
  EXPECT_FALSE(first->held());
  {
    auto again = lock_journal(file);
    ASSERT_TRUE(again.has_value());
    EXPECT_FALSE(lock_journal(file).has_value());
  }
  auto last = lock_journal(file);
  ASSERT_TRUE(last.has_value());
  JournalLock moved = std::move(*last);
  EXPECT_FALSE(lock_journal(file).has_value()) << "a moved lock is still held";
}

TEST(Journal, TheLockFileCannotBeAJournalAndAStaleOneBlocksNobody) {
  TempDir dir;
  const fs::path file = journal_file(dir.path(), "earthbank.test", 1, "S-1");
  fs::path lock_file = file;
  lock_file += ".lock";
  EXPECT_NE(lock_file.extension(), ".json");
  // What a process that died holding it leaves: the file, and no lock.
  spit(lock_file, "left by a dead process");
  const auto lock = lock_journal(file);
  EXPECT_TRUE(lock.has_value()) << (lock ? "" : to_string(lock.error()));
  // And holding it changes nothing about reading and saving the journal.
  Journal j = sample_journal();
  ASSERT_TRUE(save_journal(j, file).has_value());
  const auto back = load_journal(file);
  ASSERT_TRUE(back.has_value() && back->has_value());
}

TEST(Journal, EmptyMeansNothingIsRecorded) {
  Journal j;
  j.hash = "x";
  EXPECT_TRUE(j.empty());
  j.in_doubt = "sample";
  EXPECT_FALSE(j.empty());
  j.in_doubt.reset();
  j.measurements["0001"] = 5;
  EXPECT_FALSE(j.empty());
}

}  // namespace
}  // namespace pychron::earthbank
