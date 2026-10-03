// Source url normalization and deterministic import ids.

#include <gtest/gtest.h>

#include <filesystem>
#include <set>

#include "pychron/ingest/ids.hpp"

using namespace pychron::ingest;
using pychron::persistence::ImportSourceKind;
using pychron::persistence::Uuid;

TEST(IngestIds, NormalizeUrl) {
  EXPECT_EQ(normalize_source_url("https://GitHub.com/NMGRLData/Foo.git/"), "https://github.com/NMGRLData/Foo");
  EXPECT_EQ(normalize_source_url("git@GitHub.com:NMGRLData/Foo.git"), "git@github.com:NMGRLData/Foo");
  EXPECT_EQ(normalize_source_url("HTTPS://User@GitHub.com:8443/NMGRLData/Foo"),
            "https://User@github.com:8443/NMGRLData/Foo");
  EXPECT_EQ(normalize_source_url("ssh://git@Host.Example/Data/Foo.git"), "ssh://git@host.example/Data/Foo");
}

TEST(IngestIds, NormalizeLocalPath) {
  const auto cwd = std::filesystem::current_path();
  // A local path is made absolute; its case and a ".git" suffix are kept.
  EXPECT_EQ(normalize_source_url("Repos/Foo/"), (cwd / "Repos" / "Foo").string());
  EXPECT_EQ(normalize_source_url((cwd / "Repos" / "Foo.git").string()), (cwd / "Repos" / "Foo.git").string());
  EXPECT_EQ(normalize_source_url("Repos/./Foo"), normalize_source_url("Repos/Foo/"));
}

TEST(IngestIds, IdsAreStableAndDistinct) {
  const std::string u = "https://github.com/NMGRLData/Foo";
  const std::string c = "0123456789abcdef0123456789abcdef01234567";

  const std::vector<Uuid> ids = {
      source_id(ImportSourceKind::ProjectRepo, u, "main"),
      changeset_id(u, c),
      collection_changeset_id(u, c, "a"),
      revision_id(u, c, "a"),
      catalog_id("material", "sanidine\n"),
      derived_analysis_id(u, "66573-01A"),
      conflict_id(u, c, "a"),
  };
  for (const auto& id : ids) EXPECT_EQ(id.version(), 5);
  EXPECT_EQ(std::set<Uuid>(ids.begin(), ids.end()).size(), ids.size());

  EXPECT_EQ(source_id(ImportSourceKind::ProjectRepo, u, "main"), ids[0]);
  EXPECT_EQ(changeset_id(u, c), ids[1]);
  EXPECT_EQ(collection_changeset_id(u, c, "a"), ids[2]);
  EXPECT_EQ(revision_id(u, c, "a"), ids[3]);
  EXPECT_EQ(catalog_id("material", "sanidine\n"), ids[4]);
  EXPECT_EQ(derived_analysis_id(u, "66573-01A"), ids[5]);
  EXPECT_EQ(conflict_id(u, c, "a"), ids[6]);

  EXPECT_NE(revision_id(u, c, "a"), revision_id(u, c, "b"));
  EXPECT_NE(changeset_id(u, c), revision_id(u, c, ""));
  EXPECT_NE(source_id(ImportSourceKind::ProjectRepo, u, "main"), source_id(ImportSourceKind::MetaRepo, u, "main"));
  EXPECT_NE(source_id(ImportSourceKind::ProjectRepo, u, "main"), source_id(ImportSourceKind::ProjectRepo, u, "dev"));
}

TEST(IngestIds, KnownValue) {
  // Pinned: changing the namespace or the name layout would re-key every import.
  const auto ns = Uuid::parse(kImportNamespace);
  ASSERT_TRUE(ns.has_value());
  EXPECT_EQ(changeset_id("u", "c"), Uuid::v5(*ns, "changeset\nu\nc"));
  EXPECT_EQ(revision_id("u", "c", "p"), Uuid::v5(*ns, "revision\nu\nc\np"));
  EXPECT_EQ(source_id(ImportSourceKind::LegacyDb, "u", "b"), Uuid::v5(*ns, "source\nlegacy_db\nu\nb"));
  EXPECT_EQ(catalog_id("t", "k"), Uuid::v5(*ns, "catalog\nt\nk"));
  EXPECT_EQ(derived_analysis_id("u", "r"), Uuid::v5(*ns, "analysis\nu\nr"));
  EXPECT_EQ(conflict_id("u", "c", "p"), Uuid::v5(*ns, "conflict\nu\nc\np"));
}
