// classify_path: one assertion per row of the table in
// tests/dvc/fixtures/README.md section 3.2, and per prefix length of 3.1.

#include <gtest/gtest.h>

#include "pychron/dvc/legacy_layout.hpp"

namespace pychron::dvc {
namespace {

void expect_path(std::string_view path, FileKind kind, std::string_view key, bool uuid = false) {
  const PathInfo info = classify_path(path);
  EXPECT_EQ(info.kind, kind) << path;
  EXPECT_EQ(info.key, key) << path;
  EXPECT_EQ(info.key_is_uuid, uuid) << path;
}

TEST(Layout, ClassifiesEveryFixturePath) {
  // project/IR1010 (README 2.1)
  expect_path("660/52-01E.json", FileKind::Record, "66052-01E");
  expect_path("660/.data/52-01E.dat.json", FileKind::Data, "66052-01E");
  expect_path("660/extraction/52-01E.extr.json", FileKind::Extraction, "66052-01E");
  expect_path("660/intercepts/52-01E.inte.json", FileKind::Intercepts, "66052-01E");
  expect_path("660/baselines/52-01E.base.json", FileKind::Baselines, "66052-01E");
  expect_path("660/blanks/52-01E.blan.json", FileKind::Blanks, "66052-01E");
  expect_path("660/icfactors/52-01E.icfa.json", FileKind::IcFactors, "66052-01E");
  expect_path("660/tags/52-01E.tags.json", FileKind::Tags, "66052-01E");
  expect_path("660/peakcenter/52-01A.peak.json", FileKind::PeakCenter, "66052-01A");
  expect_path("6a9b4615cd24138b6ce541f75240dc4091378bb1.json", FileKind::Spectrometer,
              "6a9b4615cd24138b6ce541f75240dc4091378bb1");
  // project/Felix_blank180 (README 2.2): prefix of 5, file names begin with '-'.
  expect_path("bu-FD/-F-789.json", FileKind::Record, "bu-FD-F-789");
  expect_path("bu-FD/.data/-F-789.dat.json", FileKind::Data, "bu-FD-F-789");
  expect_path("bu-FD/extraction/-F-789.extr.json", FileKind::Extraction, "bu-FD-F-789");
  expect_path("bu-FD/intercepts/-F-789.inte.json", FileKind::Intercepts, "bu-FD-F-789");
  expect_path("bu-FD/baselines/-F-789.base.json", FileKind::Baselines, "bu-FD-F-789");
  expect_path("bu-FD/blanks/-F-789.blan.json", FileKind::Blanks, "bu-FD-F-789");
  expect_path("bu-FD/icfactors/-F-789.icfa.json", FileKind::IcFactors, "bu-FD-F-789");
  expect_path("fad234d16fee6c00a96e96e15edccd97faba9521.json", FileKind::Spectrometer,
              "fad234d16fee6c00a96e96e15edccd97faba9521");
  // ia/ (README 2.3): both file names.
  expect_path("660/ia/52.ia.json", FileKind::InterpretedAge, "66052");
  expect_path("694/ia/21_00000.ia.json", FileKind::InterpretedAge, "69421_00000");
}

TEST(Layout, ClassifiesTheKindsWithoutAFixture) {
  // README 3.2 rows seen only in the source or in path listings.
  expect_path("660/monitor/52-01E.moni.json", FileKind::Monitor, "66052-01E");
  expect_path("660/cosmogenic/52-01E.cosm.json", FileKind::Cosmogenic, "66052-01E");
  expect_path("656/logs/02-02A.logs.log", FileKind::Ignored, "");
  expect_path("a-01/logs/-F-2505.logs.log", FileKind::Ignored, "");
  expect_path("NM-312.F.production.json", FileKind::FrozenProduction, "NM-312.F");
  expect_path("README.md", FileKind::Ignored, "");
  // The `reduction/` root for tags and interpreted ages.
  expect_path("reduction/660/tags/52-01E.tags.json", FileKind::Tags, "66052-01E");
  expect_path("reduction/694/ia/21_00000.ia.json", FileKind::InterpretedAge, "69421_00000");
}

TEST(Layout, KeyIsPrefixJoinedToStemWhateverThePrefixLength) {
  // README 3.1: 3, 5, 4 and 2 characters.
  expect_path("660/52-01E.json", FileKind::Record, "66052-01E");
  expect_path("bu-FD/-F-789.json", FileKind::Record, "bu-FD-F-789");
  expect_path("a-01/-F-2505.json", FileKind::Record, "a-01-F-2505");
  expect_path("00/c63177-34d6-4de1-9e2b-e63b1a149436.json", FileKind::Record,
              "00c63177-34d6-4de1-9e2b-e63b1a149436", true);
  expect_path("00/extraction/c63177-34d6-4de1-9e2b-e63b1a149436.extr.json", FileKind::Extraction,
              "00c63177-34d6-4de1-9e2b-e63b1a149436", true);
  // The uuid read fallbacks (source only): prefixes of 5 and 3.
  expect_path("00c63/177-34d6-4de1-9e2b-e63b1a149436.json", FileKind::Record,
              "00c63177-34d6-4de1-9e2b-e63b1a149436", true);
  expect_path("00c/.data/63177-34d6-4de1-9e2b-e63b1a149436.dat.json", FileKind::Data,
              "00c63177-34d6-4de1-9e2b-e63b1a149436", true);
  // A runid is not a uuid, and neither is an interpreted-age name.
  EXPECT_FALSE(classify_path("660/52-01E.json").key_is_uuid);
  EXPECT_FALSE(classify_path("694/ia/21_00000.ia.json").key_is_uuid);
}

TEST(Layout, OnlyAPathMatchingNoPatternIsUnknown) {
  expect_path("664/weird/57-01A.xyz.json", FileKind::Unknown, "");
  // Directory and suffix must agree.
  expect_path("660/intercepts/52-01E.base.json", FileKind::Unknown, "");
  expect_path("660/.data/52-01E.data.json", FileKind::Unknown, "");  // the suffix is .dat.json
  expect_path("660/intercepts/52-01E.json", FileKind::Unknown, "");
  expect_path("660/52-01E.inte.json", FileKind::Unknown, "");
  expect_path("660/52-01E.txt", FileKind::Unknown, "");
  expect_path("660/logs/52-01E.logs.json", FileKind::Unknown, "");
  // Too deep, too shallow, or empty parts.
  expect_path("a/b/intercepts/52-01E.inte.json", FileKind::Unknown, "");
  expect_path("reduction/660/intercepts/52-01E.inte.json", FileKind::Unknown, "");
  expect_path("660/intercepts/.inte.json", FileKind::Unknown, "");
  expect_path("660/.json", FileKind::Unknown, "");
  expect_path("/52-01E.json", FileKind::Unknown, "");
  expect_path("", FileKind::Unknown, "");
  // Root files that are not a known kind. README 3.2: a frozen flux file
  // `<irradiation>.json` and a per-analysis frozen production are source
  // only and have no ruling; they are reported, not guessed.
  expect_path("NM-293.json", FileKind::Unknown, "");
  expect_path("660/productions/52-01E.prod.json", FileKind::Unknown, "");
  expect_path("6a9b4615cd24138b6ce541f75240dc4091378bb.json", FileKind::Unknown, "");    // 39 hex
  expect_path("6a9b4615cd24138b6ce541f75240dc4091378bbg.json", FileKind::Unknown, "");   // not hex
  expect_path("production.json", FileKind::Unknown, "");
  expect_path("NM-312.production.json", FileKind::Unknown, "");  // no level
  expect_path("docs/README.md", FileKind::Unknown, "");
  expect_path("docs/.gitignore", FileKind::Unknown, "");
  expect_path("notes.txt", FileKind::Unknown, "");
}

TEST(Layout, RootDotfilesAndReadmesAreIgnored) {
  // Not part of the data: git's own files and whatever README the host made.
  expect_path(".gitignore", FileKind::Ignored, "");
  expect_path(".gitattributes", FileKind::Ignored, "");
  expect_path(".production.json", FileKind::Ignored, "");  // a dotfile, whatever follows
  expect_path("README", FileKind::Ignored, "");
  expect_path("README.md", FileKind::Ignored, "");
  expect_path("README.rst", FileKind::Ignored, "");
  // Only at the root, and only that name.
  expect_path("660/README.md", FileKind::Unknown, "");
  expect_path("readme.md", FileKind::Unknown, "");
}

TEST(Layout, SplitFrozenProductionKey) {
  const auto k = split_frozen_production_key(classify_path("NM-312.F.production.json").key);
  EXPECT_EQ(k.irradiation, "NM-312");
  EXPECT_EQ(k.level, "F");
  // An irradiation name may itself contain a dot: split at the last one.
  const auto d = split_frozen_production_key("NM-1.5.AB");
  EXPECT_EQ(d.irradiation, "NM-1.5");
  EXPECT_EQ(d.level, "AB");
}

TEST(Layout, MakeRunid) {
  // README 5.1: 0 = A, 4 = E, 26 = AA; null means no step.
  EXPECT_EQ(make_runid("66052", 1, 4), "66052-01E");
  EXPECT_EQ(make_runid("66052", 1, 0), "66052-01A");
  EXPECT_EQ(make_runid("66052", 1, 25), "66052-01Z");
  EXPECT_EQ(make_runid("66052", 1, 26), "66052-01AA");
  EXPECT_EQ(make_runid("66052", 12, 27), "66052-12AB");
  EXPECT_EQ(make_runid("bu-FD-F", 789, -1), "bu-FD-F-789");
  EXPECT_EQ(make_runid("a-01-F", 2505, -1), "a-01-F-2505");
  // One rule for the whole program: the store's.
  for (int increment : {-1, 0, 25, 26, 27, 701, 702})
    EXPECT_EQ(make_runid("66052", 7, increment), persistence::make_runid("66052", 7, increment)) << increment;
}

}  // namespace
}  // namespace pychron::dvc
