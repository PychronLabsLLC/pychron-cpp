// verify() against a real store: a scripted import, then scripted units that
// name the rows it left (or rows it did not), and a fake age function.

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "fake_adapter.hpp"
#include "pychron/ingest/ids.hpp"
#include "pychron/ingest/verify.hpp"
#include "pychron/ingest/writer.hpp"
#include "store_fixture.hpp"

using namespace pychron;
using namespace pychron::ingest;
using namespace pychron::ingest::testing;
namespace P = pychron::persistence;
namespace pd = pychron::persistence::detail;
using P::Kind;
using P::Uuid;
using P::UtcTime;
using P::testing::open_or_die;
using P::testing::TestDatabase;

namespace {

// The normalized form of description().url.
const std::string kUrl = "https://github.com/NMGRLData/Henry_Hill";
const char* const kAlice = "alice@example.org";
const std::string kAgePath = "665/ia/73.ia.json";

const Uuid kA = *Uuid::parse("11111111-1111-4111-8111-111111111111");
const Uuid kB = *Uuid::parse("22222222-2222-4222-8222-222222222222");
const Uuid kC = *Uuid::parse("33333333-3333-4333-8333-333333333333");
const Uuid kAbsent = *Uuid::parse("99999999-9999-4999-8999-999999999999");

SourceDescription description() {
  return {P::ImportSourceKind::ProjectRepo, "https://GitHub.com/NMGRLData/Henry_Hill.git/", "main", "head-sha"};
}

Uuid source_uuid() { return source_id(P::ImportSourceKind::ProjectRepo, kUrl, "main"); }

GitWho who(const char* iso) { return {"A. Author", kAlice, *UtcTime::parse(iso)}; }

std::string record_path(int aliquot) { return "665/73-0" + std::to_string(aliquot) + ".json"; }
std::string kind_path(const char* dir, int aliquot) {
  return std::string("665/") + dir + "/73-0" + std::to_string(aliquot) + ".json";
}

// Irradiation, level and position holding identifier 66573, the spectrometer
// and the repository.
std::vector<CatalogItem> lab_catalog() {
  PositionItem position;
  position.irradiation = "NM-300";
  position.level = "A";
  position.position = 1;
  position.identifier = "66573";
  return {IrradiationItem{"NM-300"},
          LevelItem{"NM-300", "A", std::nullopt, 0.5, std::nullopt},
          position,
          MassSpecItem{{"jan", "argus", "j", std::nullopt}},
          RepositoryItem{"Henry_Hill"}};
}

// Adds one analysis of identifier 66573, with its two signal blobs, to `batch`.
void add_analysis(ImportBatch& batch, Uuid uuid, int aliquot, const std::string& commit, const GitWho& author) {
  const auto signal = P::testing::series(static_cast<float>(aliquot));
  const auto baseline = P::testing::series(0);
  AnalysisItem item;
  item.ingest = std::get<P::AnalysisIngest>(P::testing::analysis_item(P::testing::Lab{}, aliquot, signal, baseline).body);
  item.ingest.analysis = uuid;
  item.ingest.changeset = Uuid{};
  item.ingest.roots.signals = item.ingest.roots.intercepts = item.ingest.roots.baselines = Uuid{};
  item.ingest.roots.blanks = item.ingest.roots.icfactors = item.ingest.roots.tags = Uuid{};
  const std::string n = std::to_string(aliquot);
  item.keys.record = {commit, record_path(aliquot), "rec-" + n};
  item.keys.signals = {commit, kind_path(".data", aliquot), "dat-" + n};
  item.keys.intercepts = {commit, kind_path("intercepts", aliquot), "int-" + n};
  item.keys.baselines = {commit, kind_path("baselines", aliquot), "bas-" + n};
  item.keys.blanks = {commit, kind_path("blanks", aliquot), "bla-" + n};
  item.keys.icfactors = {commit, kind_path("icfactors", aliquot), "icf-" + n};
  item.who = author;
  item.repositories = {"Henry_Hill"};
  batch.blobs.push_back({item.keys.signals, {std::string(P::kCodecTv), signal, 4}});
  batch.blobs.push_back({item.keys.signals, {std::string(P::kCodecTv), baseline, 4}});
  batch.analyses.push_back(std::move(item));
}

P::Intercepts intercepts(double value) {
  P::InterceptRow row;
  row.isotope = "Ar40";
  row.detector = "H1";
  row.value = value;
  row.error = 0.5;
  row.fit = "parabolic";
  return {row};
}

// A later commit that refits the intercepts of `analysis`.
ChangesetItem refit(const std::string& commit, Uuid analysis, int aliquot, double value, const GitWho& author) {
  ChangesetItem c;
  c.commit = commit;
  c.who = author;
  c.message = "refit " + commit;
  c.revisions.push_back(
      {{commit, kind_path("intercepts", aliquot), "int-" + commit}, analysis, Kind::Intercepts, intercepts(value)});
  return c;
}

void seal(std::vector<ImportBatch>& batches) {
  for (std::size_t i = 0; i < batches.size(); ++i) {
    batches[i].resume_token = "t" + std::to_string(i + 1);
    batches[i].done = static_cast<int>(i) + 1;
    batches[i].total = static_cast<int>(batches.size());
  }
}

// Three batches: the catalog and analysis A; analysis B and a refit of A; a
// refit of B and a file nothing can read.
std::vector<ImportBatch> history() {
  std::vector<ImportBatch> out(3);
  out[0].catalog = lab_catalog();
  add_analysis(out[0], kA, 1, "c1", who("2016-03-04T05:06:07Z"));
  add_analysis(out[1], kB, 2, "c2", who("2016-03-05T00:00:00Z"));
  out[1].changesets.push_back(refit("c3", kA, 1, 101.5, who("2016-03-06T00:00:00Z")));
  out[2].changesets.push_back(refit("c4", kB, 2, 55.5, who("2016-03-07T00:00:00Z")));
  out[2].conflicts.push_back({{"c4", "notes.txt", "blob-notes"},
                              std::nullopt,
                              P::ConflictKind::Unparseable,
                              sha256(std::string_view{"notes"}),
                              R"({"reason":"unknown file"})"});
  seal(out);
  return out;
}

Evidence recorded(const SourceKey& key) { return {Evidence::Kind::Revision, key.commit, key.path}; }
Evidence analysis_at(const SourceKey& key, Uuid analysis) {
  return {Evidence::Kind::Analysis, key.commit, key.path, analysis};
}

SourceUnit unit(const SourceKey& key, UnitDisposition disposition, std::vector<Evidence> evidence = {}) {
  SourceUnit u;
  u.commit = key.commit;
  u.path = key.path;
  u.blob_sha = key.blob_sha;
  u.disposition = disposition;
  u.evidence = std::move(evidence);
  return u;
}

// What an adapter would say of the files the batches name: every file of an
// analysis, every revision, every conflict.
std::vector<SourceUnit> units_of(const std::vector<ImportBatch>& batches) {
  std::vector<SourceUnit> out;
  for (const auto& batch : batches) {
    for (const auto& a : batch.analyses) {
      out.push_back(unit(a.keys.record, UnitDisposition::Imported, {analysis_at(a.keys.record, a.ingest.analysis)}));
      for (const SourceKey* key :
           {&a.keys.signals, &a.keys.intercepts, &a.keys.baselines, &a.keys.blanks, &a.keys.icfactors})
        out.push_back(unit(*key, UnitDisposition::Imported, {recorded(*key)}));
    }
    for (const auto& c : batch.changesets)
      for (const auto& r : c.revisions) {
        out.push_back(unit(r.key, UnitDisposition::Imported, {recorded(r.key)}));
        if (std::holds_alternative<InterpretedAgeKey>(r.subject))
          out.back().interpreted_age = std::get<InterpretedAgeKey>(r.subject).name;
      }
    for (const auto& c : batch.conflicts)
      out.push_back(unit(c.key, UnitDisposition::Conflict, {{Evidence::Kind::Conflict, c.key.commit, c.key.path}}));
  }
  return out;
}

// One legacy member of an interpreted-age document.
struct Member {
  std::string uuid;  // empty: the entry has none
  std::string record_id;
  std::string age = "null", age_err = "null";  // JSON text
  std::string age_err_wo_j = {};               // JSON text; empty: the entry has none
};

// `errors_include_j`: the file's include_j_error_in_individual_analyses, JSON
// text; empty: the file does not say. Most tests are not about which error is
// compared: their files say "false", so age_err is the error without J.
std::string age_document(const std::vector<Member>& members, const std::string& errors_include_j = "false") {
  std::string doc = R"({"name":"66573 plateau",)";
  if (!errors_include_j.empty()) doc += "\"include_j_error_in_individual_analyses\":" + errors_include_j + ",";
  doc += R"("analyses":[)";
  for (std::size_t i = 0; i < members.size(); ++i) {
    const auto& m = members[i];
    if (i) doc += ",";
    doc += "{";
    if (!m.uuid.empty()) doc += "\"uuid\":\"" + m.uuid + "\",";
    if (!m.age_err_wo_j.empty()) doc += "\"age_err_wo_j\":" + m.age_err_wo_j + ",";
    doc += "\"record_id\":\"" + m.record_id + "\",\"age\":" + m.age + ",\"age_err\":" + m.age_err + "}";
  }
  return doc + "]}";
}

// A commit that saves the interpreted age at kAgePath.
ImportBatch age_batch(const std::string& commit, const char* iso, const std::vector<Member>& members,
                      const std::string& document = {}) {
  ImportBatch b;
  b.catalog.push_back(InterpretedAgeItem{kAgePath, "66573 plateau", "66573", "Henry_Hill"});
  P::InterpretedAgeValue value;
  value.age = 28.2;
  value.age_err = 0.05;
  value.doc_json = document.empty() ? age_document(members) : document;
  std::set<Uuid> listed;  // a member row once, however often the document lists it
  for (const auto& m : members)
    if (const auto uuid = Uuid::parse(m.uuid); uuid && listed.insert(*uuid).second)
      value.members.push_back({*uuid, m.record_id, std::nullopt, std::nullopt});
  ChangesetItem c;
  c.commit = commit;
  c.who = who(iso);
  c.message = "<IA> " + commit;
  c.revisions.push_back({{commit, kAgePath, "ia-" + commit}, InterpretedAgeKey{kAgePath}, Kind::InterpretedAge, value});
  b.changesets.push_back(std::move(c));
  return b;
}

WriterConfig config() {
  WriterConfig c;
  c.importer_version = "pychron-import/test";
  c.lab_time_zone = "America/Denver";
  return c;
}

std::string err(const Error& e) { return to_string(e); }

// A fresh database: the store under test and a white-box connection to it.
struct World {
  explicit World(const std::string& engine) : database(engine, true) {
    store = open_or_die(database.url());
    if (!store) return;
    client = *store->register_client({"import-1", "importer", std::nullopt, "test"});
    auto opened = pd::Db::open(P::StoreConfig{database.url(), false});
    if (opened) db = std::move(*opened);
  }

  long long count(const char* table) {
    auto row = db->select_one(QStringLiteral("SELECT count(*) AS n FROM %1").arg(QString::fromUtf8(table)));
    return row && *row ? (*row)->value("n").toLongLong() : -1;
  }

  std::map<std::string, long long> counts() {
    std::map<std::string, long long> out;
    for (const char* t : {"analysis", "signal_blob", "changeset", "revision", "head_move", "import_provenance",
                          "import_conflict", "repository_member", "change_log", "app_user", "identifier",
                          "import_source"})
      out[t] = count(t);
    return out;
  }

  // Declared first so it is destroyed last: the connections point into it.
  TestDatabase database;
  std::unique_ptr<P::IStore> store;
  Uuid client;
  std::unique_ptr<pd::Db> db;
};

class VerifierTest : public ::testing::TestWithParam<std::string> {
 protected:
  void SetUp() override {
    world_ = std::make_unique<World>(GetParam());
    ASSERT_TRUE(world_->store);
    ASSERT_TRUE(world_->db);
  }

  P::IStore& store() { return *world_->store; }

  // Imports `batches` (all of them, or the first `max_batches`).
  void run_import(const std::vector<ImportBatch>& batches, std::optional<int> max_batches = std::nullopt) {
    FakeAdapter adapter(description(), batches);
    adapter.honour_token(true);
    BatchWriter writer(store(), world_->client, config());
    auto stats = writer.run(adapter, max_batches, {}, {});
    ASSERT_TRUE(stats) << err(stats.error());
  }

  // Verifies the source the batches describe, with the units given.
  VerifyReport check(const std::vector<ImportBatch>& batches, std::vector<SourceUnit> units, const AgeFn& age_fn = {},
                     VerifyOptions options = {}) {
    FakeAdapter adapter(description(), batches);
    adapter.honour_token(true);
    adapter.units(std::move(units));
    auto report = verify(store(), world_->client, adapter, config(), age_fn, options);
    EXPECT_TRUE(report) << (report ? "" : err(report.error()));
    return report ? *report : VerifyReport{};
  }

  P::ImportSourceInfo source() {
    auto all = store().import_sources();
    EXPECT_TRUE(all && all->size() == 1);
    return all && !all->empty() ? all->front() : P::ImportSourceInfo{};
  }

  P::ImportConflictRow conflict(Uuid id) {
    auto row = store().import_conflict(id);
    EXPECT_TRUE(row && *row) << "no conflict " << id.str();
    return row && *row ? **row : P::ImportConflictRow{};
  }

  void resolve(Uuid id, const char* resolution) {
    auto uow = store().begin_import_batch(source_uuid(), world_->client);
    ASSERT_TRUE(uow);
    ASSERT_TRUE((*uow)->resolve_conflict(id, resolution));
    ASSERT_TRUE((*uow)->commit());
  }

  std::unique_ptr<World> world_;
};

// The history with an interpreted age of A and B saved at c5.
std::vector<ImportBatch> history_with_age(const std::vector<Member>& members, const std::string& document = {}) {
  auto batches = history();
  batches.push_back(age_batch("c5", "2016-03-08T00:00:00Z", members, document));
  seal(batches);
  return batches;
}

Uuid parity_conflict(Uuid analysis) {
  return conflict_id(kUrl, "parity", analysis.str() + "/" + interpreted_age_id(kUrl, kAgePath).str());
}

AgeFn ages(std::map<Uuid, ParityAge> by_analysis) {
  return [by_analysis = std::move(by_analysis)](Uuid analysis, const AsOf&) -> Result<ParityAge> {
    const auto it = by_analysis.find(analysis);
    if (it == by_analysis.end()) return ParityAge{NotComparable{"no age scripted"}};
    return it->second;
  };
}

}  // namespace

// ---------------------------------------------------------------- accounting

TEST_P(VerifierTest, CleanImportIsOk) {
  const auto batches = history();
  run_import(batches);
  // Resolve the one conflict the history holds: only pending ones fail verify.
  resolve(conflict_id(kUrl, "c4", "notes.txt"), "ignored");

  const auto report = check(batches, units_of(batches));
  EXPECT_TRUE(report.ok());
  EXPECT_EQ(report.units, 15);  // two analyses of six files, two refits, one conflict
  EXPECT_EQ(report.ignored, 0);
  EXPECT_TRUE(report.unaccounted.empty());
  EXPECT_EQ(report.would_write, 0);
  EXPECT_EQ(report.replay_would_write, 0);
  EXPECT_EQ(report.pending_blocking, 0);
  EXPECT_EQ(report.pending_warnings, 0);
  EXPECT_EQ(report.parity_pass + report.parity_fail + report.parity_not_comparable, 0);
  EXPECT_TRUE(report.source.registered);
  EXPECT_EQ(report.source.status, "finished");
  EXPECT_EQ(report.source.done, 3);
  EXPECT_EQ(report.source.total, 3);
  EXPECT_EQ(report.source.stored_head, std::optional<std::string>{"head-sha"});
  EXPECT_EQ(report.source.current_head, "head-sha");
}

// Everything else can look right for a source that was not imported, not to
// its end, or that has moved since: rows other sources made satisfy natural
// keys, and a dry run does not count catalog rows (spec 10.28).
TEST_P(VerifierTest, NeedsAFinishedImportOfTheSourceAsItIsNow) {
  std::vector<ImportBatch> batches(2);
  batches[0].catalog = lab_catalog();
  batches[1].catalog = {ExtractDeviceItem{"Fusions CO2"}};
  seal(batches);
  std::vector<SourceUnit> units;
  for (const auto& batch : batches)
    for (const auto& item : batch.catalog)
      units.push_back(unit({"dump", "Tbl.jsonl#" + std::to_string(units.size()), "line"}, UnitDisposition::Imported,
                           {{Evidence::Kind::CatalogRow, {}, {}, {}, {}, item}}));
  const auto verified = [&](const SourceDescription& as) {
    FakeAdapter adapter(as, batches);
    adapter.honour_token(true);
    adapter.units(units);
    auto report = verify(store(), world_->client, adapter, config(), {}, {});
    EXPECT_TRUE(report) << (report ? "" : err(report.error()));
    return report ? *report : VerifyReport{};
  };
  const auto clean_but_for_the_source = [](const VerifyReport& r) {
    return r.unaccounted.empty() && r.would_write == 0 && r.replay_would_write == 0 && r.pending_blocking == 0;
  };

  // The rows exist: another source brought them. This source was never imported.
  {
    SourceDescription other = description();
    other.url = "https://github.com/NMGRLData/Other";
    FakeAdapter adapter(other, batches);
    BatchWriter writer(store(), world_->client, config());
    ASSERT_TRUE(writer.run(adapter, std::nullopt, {}, {}));
  }
  auto report = verified(description());
  EXPECT_TRUE(clean_but_for_the_source(report));
  EXPECT_FALSE(report.source.registered);
  EXPECT_EQ(report.source.status, "");
  EXPECT_FALSE(report.ok());

  // Imported, but the last run stopped before the end of the stream was seen.
  run_import(batches, 1);
  report = verified(description());
  EXPECT_TRUE(report.source.registered);
  EXPECT_EQ(report.source.status, "paused");
  EXPECT_EQ(report.source.done, 1);
  EXPECT_EQ(report.source.total, 2);
  EXPECT_TRUE(clean_but_for_the_source(report));
  EXPECT_FALSE(report.ok());

  // Finished, and the source is what was imported.
  run_import(batches);
  report = verified(description());
  EXPECT_EQ(report.source.status, "finished");
  EXPECT_TRUE(report.source.finished_and_current());
  EXPECT_TRUE(report.ok());

  // Finished, but the source has moved since: what was verified is not what is there.
  SourceDescription moved = description();
  moved.head = "head-2";
  report = verified(moved);
  EXPECT_EQ(report.source.status, "finished");
  EXPECT_EQ(report.source.stored_head, std::optional<std::string>{"head-sha"});
  EXPECT_EQ(report.source.current_head, "head-2");
  EXPECT_TRUE(clean_but_for_the_source(report));
  EXPECT_FALSE(report.ok());
}

TEST_P(VerifierTest, MissingProvenanceIsUnaccounted) {
  const auto batches = history();
  run_import(batches);
  resolve(conflict_id(kUrl, "c4", "notes.txt"), "ignored");

  auto units = units_of(batches);
  const SourceKey extra{"c4", "665/tags/73-02.json", "tag-2"};
  units.push_back(unit(extra, UnitDisposition::Imported, {recorded(extra)}));
  const auto report = check(batches, units);
  EXPECT_FALSE(report.ok());
  ASSERT_EQ(report.unaccounted.size(), 1u);
  EXPECT_EQ(report.unaccounted[0].unit.commit, "c4");
  EXPECT_EQ(report.unaccounted[0].unit.path, "665/tags/73-02.json");
  ASSERT_EQ(report.unaccounted[0].missing.size(), 1u);
  EXPECT_EQ(report.unaccounted[0].missing[0].kind, Evidence::Kind::Revision);
  EXPECT_EQ(report.would_write, 0);
}

TEST_P(VerifierTest, DeletedAndConflictedUnitsAreAccounted) {
  const auto batches = history();
  run_import(batches);
  resolve(conflict_id(kUrl, "c4", "notes.txt"), "ignored");

  auto units = units_of(batches);
  SourceUnit deleted = unit({"c5", "665/73-01.json", ""}, UnitDisposition::Removed);
  deleted.deleted = true;
  units.push_back(deleted);
  EXPECT_TRUE(check(batches, units).ok());

  // A unit said to be a conflict needs its conflict row: by id, whatever the bytes.
  units.push_back(unit({"c9", "notes.txt", "blob-notes"}, UnitDisposition::Conflict,
                       {{Evidence::Kind::Conflict, "c9", "notes.txt"}}));
  // A deletion that names a row needs that row.
  SourceUnit removal = unit({"c9", "NM-300/A.json", ""}, UnitDisposition::Removed, {recorded({"c9", "NM-300/A.json#1", ""})});
  removal.deleted = true;
  units.push_back(removal);
  // "Removed" accounts for a deletion, not for a file that is there.
  units.push_back(unit({"c9", "665/73-01.json", "rec-1b"}, UnitDisposition::Removed));
  const auto report = check(batches, units);
  EXPECT_FALSE(report.ok());
  ASSERT_EQ(report.unaccounted.size(), 3u);
  EXPECT_EQ(report.unaccounted[0].unit.path, "665/73-01.json");  // sorted by path
  EXPECT_FALSE(report.unaccounted[0].unit.deleted);
  EXPECT_EQ(report.unaccounted[1].unit.path, "NM-300/A.json");
  EXPECT_EQ(report.unaccounted[2].unit.path, "notes.txt");
  EXPECT_EQ(report.unaccounted[2].unit.commit, "c9");
}

// A unit is accounted for by a row at its own commit, or by the rows of the
// one earlier unit it repeats: never by "that blob is somewhere" (spec 10.27).
TEST_P(VerifierTest, ARepeatPointsAtTheUnitItRepeats) {
  const auto batches = history();
  run_import(batches);
  resolve(conflict_id(kUrl, "c4", "notes.txt"), "ignored");

  // c3's refit of A, seen again at c8 with the same blob (a merge repeating a side).
  const std::string path = kind_path("intercepts", 1);
  auto units = units_of(batches);
  SourceUnit repeat = unit({"c8", path, "int-c3"}, UnitDisposition::Unchanged, {recorded({"c3", path, "int-c3"})});
  repeat.repeats = "c3";
  units.push_back(repeat);
  EXPECT_TRUE(check(batches, units).ok());

  // The same content at another commit said to be a revision of its own: it
  // needs its own row, whatever blob the path has elsewhere.
  units.push_back(unit({"c9", path, "int-c3"}, UnitDisposition::Imported, {recorded({"c9", path, "int-c3"})}));
  // A repeat of a unit that was never imported.
  units.push_back(unit({"c10", path, "int-zz"}, UnitDisposition::Unchanged, {recorded({"c7", path, "int-zz"})}));
  const auto report = check(batches, units);
  ASSERT_EQ(report.unaccounted.size(), 2u);
  EXPECT_EQ(report.unaccounted[0].unit.commit, "c10");
  EXPECT_EQ(report.unaccounted[0].missing[0].commit, "c7");
  EXPECT_EQ(report.unaccounted[1].unit.commit, "c9");
}

// A provenance row is not enough: the revision or analysis it is the
// provenance of must be there too. (The store cannot be brought into that
// state by deleting: revision rows are append-only by trigger, and an analysis
// row is held by the foreign keys of its meta, isotope and member rows. The
// rows are written here instead, for entities that do not exist.)
TEST_P(VerifierTest, AProvenanceRowWithoutItsEntityIsNotEvidence) {
  const auto batches = history();
  run_import(batches);
  resolve(conflict_id(kUrl, "c4", "notes.txt"), "ignored");
  const std::string path = kind_path("intercepts", 1);
  const Uuid ghost = *Uuid::parse("44444444-4444-4444-8444-444444444444");
  {
    auto uow = store().begin_import_batch(source_uuid(), world_->client);
    ASSERT_TRUE(uow);
    const auto at = *UtcTime::parse("2016-03-09T00:00:00Z");
    ASSERT_TRUE((*uow)->add_provenance({"revision", ghost, path, "c9", "int-c9", "A <a@x>", at, std::nullopt}));
    ASSERT_TRUE((*uow)->add_provenance({"analysis", kC, record_path(9), "c9", "rec-9", "A <a@x>", at, std::nullopt}));
    ASSERT_TRUE((*uow)->commit());
  }
  ASSERT_TRUE(*store().has_provenance(source_uuid(), "c9", path));
  ASSERT_TRUE(*store().has_provenance(source_uuid(), "c9", record_path(9)));

  auto units = units_of(batches);
  units.push_back(unit({"c9", path, "int-c9"}, UnitDisposition::Imported, {recorded({"c9", path, "int-c9"})}));
  units.push_back(unit({"c9", record_path(9), "rec-9"}, UnitDisposition::Imported,
                       {analysis_at({"c9", record_path(9), "rec-9"}, kC)}));
  units.push_back(unit({"c9", "copy/73-09.json", "rec-9"}, UnitDisposition::Folded, {{Evidence::Kind::Entity, {}, {}, kC}}));
  const auto report = check(batches, units);
  EXPECT_FALSE(report.ok());
  ASSERT_EQ(report.unaccounted.size(), 3u);
  for (const auto& open : report.unaccounted) EXPECT_EQ(open.unit.commit, "c9");
}

// An adapter cannot account for a unit by classifying it: Folded and
// Unchanged must point at a row, and what it cannot classify is reported.
TEST_P(VerifierTest, FoldedUnchangedAndUnclassifiedNeedARow) {
  const auto batches = history();
  run_import(batches);
  resolve(conflict_id(kUrl, "c4", "notes.txt"), "ignored");

  const SourceKey record{"c1", record_path(1), "rec-1"};
  auto units = units_of(batches);
  units.push_back(unit({"c1", "665/extraction/73-01.json", "ext-1"}, UnitDisposition::Folded, {analysis_at(record, kA)}));
  units.push_back(unit({"c1", "logs/73-01.logs.log", "log-1"}, UnitDisposition::Ignored));
  auto report = check(batches, units);
  EXPECT_TRUE(report.ok());
  EXPECT_EQ(report.units, 17);
  EXPECT_EQ(report.ignored, 1);

  units.push_back(unit({"c1", "665/peakcenter/73-01.json", "pc-1"}, UnitDisposition::Folded));
  units.push_back(unit({"c2", "665/peakcenter/73-01.json", "pc-1"}, UnitDisposition::Unchanged));
  units.push_back(unit({"c3", "665/peakcenter/73-01.json", "pc-2"}, UnitDisposition::Unclassified, {analysis_at(record, kA)}));
  units.push_back(unit({"c4", "665/peakcenter/73-01.json", "pc-3"}, UnitDisposition::Imported));
  units.push_back(unit({"c5", "665/peakcenter/73-01.json", "pc-4"}, UnitDisposition::Conflict));
  // Folded into an analysis that is not there.
  units.push_back(unit({"c6", "665/extraction/73-09.json", "ext-9"}, UnitDisposition::Folded,
                       {analysis_at({"c6", record_path(9), "rec-9"}, kC)}));
  report = check(batches, units);
  EXPECT_FALSE(report.ok());
  ASSERT_EQ(report.unaccounted.size(), 6u);
  EXPECT_EQ(report.unaccounted[0].unit.path, "665/extraction/73-09.json");
  EXPECT_EQ(report.unaccounted[0].missing.size(), 1u);
  for (std::size_t i = 1; i < 6; ++i) {
    EXPECT_EQ(report.unaccounted[i].unit.commit, "c" + std::to_string(i));
    EXPECT_TRUE(report.unaccounted[i].missing.empty());
  }
}

TEST_P(VerifierTest, RewriteAndRemovalNotesAreEvidence) {
  auto batches = history();
  // c6 rewrites A's record without a revision; c7 notes a removed object.
  ChangesetItem sync;
  sync.commit = "c6";
  sync.who = who("2016-03-09T00:00:00Z");
  sync.message = "<SYNC>";
  sync.rewrites.push_back({record_path(1), R"({"path":")" + record_path(1) + R"(","blob":"rec-1b","changed":{}})"});
  ChangesetItem gone;
  gone.commit = "c7";
  gone.who = who("2016-03-10T00:00:00Z");
  gone.message = "drop";
  gone.kind = P::ChangesetKind::Reference;
  gone.detail_json = R"({"removed":["NM-300/productions.json#B","spectrometers/jan.sens.json"]})";
  batches.push_back({});
  batches.back().changesets = {sync, gone};
  seal(batches);
  run_import(batches);
  resolve(conflict_id(kUrl, "c4", "notes.txt"), "ignored");

  auto units = units_of(batches);
  const auto note = [](const char* commit, const std::string& path, const char* list) {
    return Evidence{Evidence::Kind::Note, commit, path, {}, list};
  };
  units.push_back(unit({"c6", record_path(1), "rec-1b"}, UnitDisposition::Imported, {note("c6", record_path(1), "rewrites")}));
  units.push_back(unit({"c7", "NM-300/productions.json", "p-2"}, UnitDisposition::Imported,
                       {note("c7", "NM-300/productions.json#B", "removed")}));
  SourceUnit deleted = unit({"c7", "spectrometers/jan.sens.json", ""}, UnitDisposition::Removed,
                            {note("c7", "spectrometers/jan.sens.json", "removed")});
  deleted.deleted = true;
  units.push_back(deleted);
  EXPECT_TRUE(check(batches, units).ok());

  // Not in that commit's list; in no list of that name; a commit without a changeset.
  units.push_back(unit({"c6", record_path(2), "rec-2b"}, UnitDisposition::Imported, {note("c6", record_path(2), "rewrites")}));
  units.push_back(unit({"c7", "x", "x"}, UnitDisposition::Imported, {note("c7", record_path(1), "rewrites")}));
  units.push_back(unit({"c8", "y", "y"}, UnitDisposition::Imported, {note("c8", record_path(1), "rewrites")}));
  const auto report = check(batches, units);
  ASSERT_EQ(report.unaccounted.size(), 3u);
  EXPECT_EQ(report.unaccounted[0].unit.path, record_path(2));
  EXPECT_EQ(report.unaccounted[1].unit.path, "x");
  EXPECT_EQ(report.unaccounted[2].unit.path, "y");
}

TEST_P(VerifierTest, EntityEvidenceIsAProvenanceRowOfThisSource) {
  const auto batches = history();
  run_import(batches);
  resolve(conflict_id(kUrl, "c4", "notes.txt"), "ignored");

  // A second copy of A under another path: nothing is stored at its key.
  auto units = units_of(batches);
  units.push_back(unit({"c4", "copy/73-01.json", "rec-1"}, UnitDisposition::Folded, {{Evidence::Kind::Entity, {}, {}, kA}}));
  EXPECT_TRUE(check(batches, units).ok());

  units.push_back(unit({"c4", "copy/73-09.json", "rec-9"}, UnitDisposition::Folded, {{Evidence::Kind::Entity, {}, {}, kC}}));
  const auto report = check(batches, units);
  ASSERT_EQ(report.unaccounted.size(), 1u);
  EXPECT_EQ(report.unaccounted[0].unit.path, "copy/73-09.json");
}

TEST_P(VerifierTest, CatalogRowsAreFoundByNaturalKey) {
  ImportBatch b;
  SampleItem sample;
  sample.fields.name = "HH-1";
  sample.project = "Henry Hill";
  sample.material = "sanidine";
  sample.pi_last_name = "Ross";
  sample.pi_first_initial = "J";
  PositionItem position;
  position.irradiation = "NM-300";
  position.level = "A";
  position.position = 1;
  position.identifier = "66573";
  LoadItem load;
  load.spec.name = "L-1";
  b.catalog = {PiItem{"Ross", "J", "NMT", std::nullopt},
               ProjectItem{"Henry Hill", "Ross", "J"},
               ProjectItem{"Orphan", std::nullopt, std::nullopt},
               MaterialItem{"sanidine", ""},
               sample,
               IrradiationItem{"NM-300"},
               LevelItem{"NM-300", "A", std::nullopt, 0.5, std::nullopt},
               position,
               UserItem{"jross"},
               MassSpecItem{{"jan", "argus", "j", std::nullopt}},
               ExtractDeviceItem{"Fusions CO2"},
               load,
               LoadPositionItem{"L-1", 3, "66573", std::nullopt, std::nullopt, std::nullopt},
               SpecialIdentifierItem{"ba-01-J", "blank_air", "jan"},
               RepositoryItem{"Henry_Hill"},
               RefObjectItem{P::RefType::LoadHolder, "221-hole", std::nullopt, std::nullopt, std::nullopt, std::nullopt}};
  std::vector<ImportBatch> batches{b};
  seal(batches);
  run_import(batches);

  const auto row = [](int line, const CatalogItem& item) {
    return unit({"dump", "Tbl.jsonl#" + std::to_string(line), "line"}, UnitDisposition::Imported,
                {{Evidence::Kind::CatalogRow, {}, {}, {}, {}, item}});
  };
  std::vector<SourceUnit> units;
  int n = 0;
  for (const auto& item : b.catalog) units.push_back(row(++n, item));
  auto report = check(batches, units);
  EXPECT_TRUE(report.ok());
  EXPECT_EQ(report.units, 16);

  // Rows that are not there: one of each shape of key.
  SampleItem other_sample = sample;
  other_sample.pi_last_name.reset();  // the project of that name without a PI holds no such sample
  other_sample.pi_first_initial.reset();
  PositionItem other_position = position;
  other_position.position = 2;
  PositionItem other_identifier = position;
  other_identifier.identifier = "66574";
  LoadItem other_load;
  other_load.spec.name = "L-2";
  const std::vector<CatalogItem> absent{PiItem{"Ross", "K", std::nullopt, std::nullopt},
                                        ProjectItem{"Henry Hill", std::nullopt, std::nullopt},
                                        ProjectItem{"Orphan", "Ross", "J"},
                                        MaterialItem{"sanidine", "fine"},
                                        other_sample,
                                        IrradiationItem{"NM-301"},
                                        LevelItem{"NM-300", "B", std::nullopt, std::nullopt, std::nullopt},
                                        other_position,
                                        other_identifier,
                                        UserItem{"nobody"},
                                        MassSpecItem{{"felix", std::nullopt, std::nullopt, std::nullopt}},
                                        ExtractDeviceItem{"Fusions Diode"},
                                        other_load,
                                        LoadPositionItem{"L-1", 4, "66573", std::nullopt, std::nullopt, std::nullopt},
                                        SpecialIdentifierItem{"bu-01-J", "blank_unknown", "jan"},
                                        RepositoryItem{"Other"},
                                        RefObjectItem{P::RefType::IrradiationHolder, "221-hole", std::nullopt, std::nullopt,
                                                      std::nullopt, std::nullopt}};
  for (const auto& item : absent) units.push_back(row(++n, item));
  // A row said to be imported with no item to find it by.
  units.push_back(unit({"dump", "Tbl.jsonl#none", "line"}, UnitDisposition::Imported, {{Evidence::Kind::CatalogRow}}));
  report = check(batches, units);
  EXPECT_FALSE(report.ok());
  EXPECT_EQ(report.unaccounted.size(), absent.size() + 1);
}

// An analysis the writer refused has a conflict per file: accounted for, and
// what makes verify fail is that the conflicts are pending.
TEST_P(VerifierTest, RefusedAnalysisIsAccountedButBlocks) {
  std::vector<ImportBatch> batches(1);
  add_analysis(batches[0], kA, 1, "c1", who("2016-03-04T05:06:07Z"));  // no catalog: identifier 66573 is unknown
  batches[0].changesets.push_back(refit("c2", kA, 1, 101.5, who("2016-03-05T00:00:00Z")));
  seal(batches);
  run_import(batches);

  const auto report = check(batches, units_of(batches));
  EXPECT_TRUE(report.unaccounted.empty());
  EXPECT_EQ(report.would_write, 0);
  EXPECT_EQ(report.replay_would_write, 0);
  EXPECT_EQ(report.pending_blocking, 7);  // six files and the refit
  EXPECT_EQ(report.pending_warnings, 0);
  EXPECT_FALSE(report.ok());
}

// ---------------------------------------------------------------- idempotence

TEST_P(VerifierTest, NotYetImportedCountsWouldWrite) {
  const auto batches = history();
  run_import(batches, 1);  // batch 1 of 3

  auto report = check(batches, units_of(batches));
  EXPECT_FALSE(report.ok());
  // Batches 2 and 3: analysis B, its two blobs share A's baseline (1 new), two
  // refits with their changesets, one conflict.
  EXPECT_EQ(report.would_write, 7);
  EXPECT_EQ(report.replay_would_write, 7);
  // B's six files, the two refits and the conflict.
  EXPECT_EQ(report.unaccounted.size(), 9u);

  run_import(batches);
  resolve(conflict_id(kUrl, "c4", "notes.txt"), "ignored");
  report = check(batches, units_of(batches));
  EXPECT_TRUE(report.ok());
}

TEST_P(VerifierTest, LeavesTokenStatusAndRowsAlone) {
  const auto batches = history();
  run_import(batches, 2);
  const auto before = source();
  ASSERT_EQ(before.status, "paused");
  const auto rows = world_->counts();

  const auto report = check(batches, units_of(batches));
  EXPECT_FALSE(report.ok());
  EXPECT_GT(report.would_write, 0);

  const auto after = source();
  EXPECT_EQ(after.status, before.status);
  EXPECT_EQ(after.progress_token, before.progress_token);
  EXPECT_EQ(after.done, before.done);
  EXPECT_EQ(after.total, before.total);
  EXPECT_EQ(after.head_sha, before.head_sha);
  EXPECT_EQ(after.finished, before.finished);
  EXPECT_EQ(world_->counts(), rows);

  // A source that was never imported is not registered by verifying it.
  World fresh(GetParam());
  ASSERT_TRUE(fresh.store);
  FakeAdapter adapter(description(), batches);
  adapter.units(units_of(batches));
  auto unseen = verify(*fresh.store, fresh.client, adapter, config(), {}, {});
  ASSERT_TRUE(unseen) << err(unseen.error());
  EXPECT_FALSE(unseen->ok());
  EXPECT_EQ(unseen->unaccounted.size(), 15u);
  EXPECT_FALSE(unseen->source.registered);
  EXPECT_EQ(fresh.count("import_source"), 0);
  EXPECT_EQ(fresh.count("analysis"), 0);
}

// ---------------------------------------------------------------- pending conflicts

TEST_P(VerifierTest, WarningsDoNotFailBlockingConflictsDo) {
  auto batches = history();
  // Conflicts that only annotate a row that was imported.
  batches[0].conflicts.push_back({{"", "catalog/identifier/66573", ""},
                                  std::nullopt,
                                  P::ConflictKind::IdentityClash,
                                  std::nullopt,
                                  R"({"synthesized":true,"table":"identifier","identifier":"66573"})"});
  batches[0].conflicts.push_back({{"c1", "SampleTbl.jsonl#7@projectID", "line"},
                                  std::nullopt,
                                  P::ConflictKind::IdentityClash,
                                  std::nullopt,
                                  R"({"imported":true,"column":"projectID"})"});
  run_import(batches);
  const Uuid unparseable = conflict_id(kUrl, "c4", "notes.txt");

  // The history's unparseable file is pending: it fails verify.
  auto units = units_of(history());
  auto report = check(batches, units);
  EXPECT_FALSE(report.ok());
  EXPECT_TRUE(report.unaccounted.empty());
  EXPECT_EQ(report.pending_blocking, 1);
  ASSERT_EQ(report.blocking_conflicts.size(), 1u);
  EXPECT_EQ(report.blocking_conflicts[0], unparseable);
  EXPECT_EQ(report.pending_warnings, 2);
  EXPECT_EQ(report.warning_conflicts.size(), 2u);

  // Superseded (or resolved any other way), it does not; the warnings stay.
  resolve(unparseable, "superseded");
  report = check(batches, units);
  EXPECT_TRUE(report.ok());
  EXPECT_EQ(report.pending_blocking, 0);
  EXPECT_EQ(report.pending_warnings, 2);

  // "imported": false is a refusal.
  batches.push_back({});
  batches.back().conflicts.push_back({{"c6", "SampleTbl.jsonl#8", "line"},
                                      std::nullopt,
                                      P::ConflictKind::IdentityClash,
                                      std::nullopt,
                                      R"({"imported":false,"reason":"projectID is missing"})"});
  seal(batches);
  run_import(batches);
  report = check(batches, units);
  EXPECT_FALSE(report.ok());
  EXPECT_EQ(report.pending_blocking, 1);
  EXPECT_EQ(report.blocking_conflicts[0], conflict_id(kUrl, "c6", "SampleTbl.jsonl#8"));
}

// Spec 10.35: a revision kept in a late_revision_not_applied conflict is
// history that could not be inserted; the head is right.
TEST_P(VerifierTest, LateRevisionIsAWarning) {
  auto batches = history();
  batches[0].conflicts.push_back({{"b1", "665/intercepts/73-01.json", "blob"},
                                  kA,
                                  P::ConflictKind::IdentityClash,
                                  std::nullopt,
                                  R"({"reason":"late_revision_not_applied","late":true,"commit":"b1"})"});
  run_import(batches);
  resolve(conflict_id(kUrl, "c4", "notes.txt"), "superseded");
  const auto report = check(batches, units_of(history()));
  EXPECT_TRUE(report.ok());
  EXPECT_EQ(report.pending_blocking, 0);
  EXPECT_EQ(report.warning_conflicts, std::vector<Uuid>{conflict_id(kUrl, "b1", "665/intercepts/73-01.json")});
}

// ---------------------------------------------------------------- age parity

TEST_P(VerifierTest, ParityPassFailNotComparable) {
  // Three members: A, B and an analysis of another repository that is not imported.
  const auto batches = history_with_age({{kA.str(), "66573-01", "28.25", "0.125"},
                                         {kB.str(), "66573-02", "28.5", "0.25"},
                                         {kAbsent.str(), "66573-03", "28.75", "0.5"}});
  run_import(batches);
  resolve(conflict_id(kUrl, "c4", "notes.txt"), "ignored");
  const auto rows = world_->counts();

  const Uuid age = interpreted_age_id(kUrl, kAgePath);
  std::vector<AsOf> asked;
  const AgeFn fn = [&](Uuid analysis, const AsOf& as_of) -> Result<ParityAge> {
    asked.push_back(as_of);
    if (analysis == kA) return ParityAge{ComputedAge{28.25, 0.125}};
    return ParityAge{ComputedAge{28.5 * (1 + 1e-5), 0.25}};  // off by 1e-5, relative: beyond the default 1e-6
  };
  const auto report = check(batches, units_of(batches), fn);
  EXPECT_FALSE(report.ok());
  EXPECT_TRUE(report.unaccounted.empty());
  EXPECT_EQ(report.parity_pass, 1);
  EXPECT_EQ(report.parity_fail, 1);
  EXPECT_EQ(report.parity_not_comparable, 1);
  EXPECT_EQ(report.not_comparable_reasons, (std::map<std::string, int>{{"analysis is not in the store", 1}}));

  // The function was asked about A and B, as of the commit that saved the age.
  ASSERT_EQ(asked.size(), 2u);
  auto revisions = store().history(age, Kind::InterpretedAge);
  ASSERT_TRUE(revisions);
  ASSERT_EQ(revisions->size(), 1u);
  for (const auto& as_of : asked) {
    EXPECT_EQ(as_of.interpreted_age, age);
    EXPECT_EQ(as_of.revision, revisions->front().uuid);
    EXPECT_EQ(as_of.changeset, changeset_id(kUrl, "c5"));
    EXPECT_EQ(as_of.source, source_uuid());
    EXPECT_EQ(as_of.commit, "c5");
    EXPECT_EQ(as_of.created, *UtcTime::parse("2016-03-08T00:00:00Z"));
  }

  // The failure: listed, and one value_mismatch conflict with both values.
  ASSERT_EQ(report.parity_failures.size(), 1u);
  const auto& failure = report.parity_failures[0];
  EXPECT_EQ(failure.analysis, kB);
  EXPECT_EQ(failure.interpreted_age, age);
  EXPECT_EQ(failure.conflict, parity_conflict(kB));
  EXPECT_DOUBLE_EQ(failure.legacy_age, 28.5);
  EXPECT_DOUBLE_EQ(failure.computed_age, 28.5 * (1 + 1e-5));
  EXPECT_NEAR(failure.age_difference, 1e-5, 1e-9);
  EXPECT_EQ(failure.age_err_difference, 0.0);

  const auto stored = conflict(parity_conflict(kB));
  EXPECT_EQ(stored.kind, P::ConflictKind::ValueMismatch);
  EXPECT_EQ(stored.resolution, "pending");
  EXPECT_EQ(stored.path, kAgePath);
  EXPECT_EQ(stored.entity, kB);
  EXPECT_EQ(stored.db_head_revision, revisions->front().uuid);
  for (const char* part : {"\"check\"", "age_parity", "\"legacy\"", "28.5", "\"computed\"", "28.5002", "0.25",
                           "\"relative_difference\"", "\"tolerance\"", "66573-02", "\"as_of\"", "\"commit\""})
    EXPECT_NE(stored.detail_json.find(part), std::string::npos) << part << " in " << stored.detail_json;
  EXPECT_NE(stored.detail_json.find(age.str()), std::string::npos);
  EXPECT_NE(stored.detail_json.find(changeset_id(kUrl, "c5").str()), std::string::npos);
  EXPECT_EQ(report.pending_blocking, 1);
  EXPECT_EQ(report.blocking_conflicts, std::vector<Uuid>{parity_conflict(kB)});

  // The conflict is all verify wrote.
  auto expected = rows;
  ++expected["import_conflict"];
  EXPECT_EQ(world_->counts(), expected);
  EXPECT_EQ(source().status, "finished");
}

TEST_P(VerifierTest, ToleranceIsRelative) {
  const auto batches = history_with_age({{kA.str(), "66573-01", "1e9", "1e3"}, {kB.str(), "66573-02", "1e-9", "0"}});
  run_import(batches);
  resolve(conflict_id(kUrl, "c4", "notes.txt"), "ignored");

  // 0.5 in 1e9 is 5e-10: inside the default 1e-6. Two zero errors are the same.
  auto report = check(batches, units_of(batches),
                      ages({{kA, ComputedAge{1e9 + 0.5, 1e3}}, {kB, ComputedAge{1e-9, 0.0}}}));
  EXPECT_EQ(report.parity_pass, 2);
  EXPECT_EQ(report.parity_fail, 0);
  EXPECT_TRUE(report.ok());

  // The error is compared too, by the same rule; and a looser tolerance passes it.
  const auto off = ages({{kA, ComputedAge{1e9, 1e3 + 1.0}}, {kB, ComputedAge{1e-9, 0.0}}});
  report = check(batches, units_of(batches), off, VerifyOptions{1e-2});
  EXPECT_EQ(report.parity_fail, 0);
  report = check(batches, units_of(batches), off);
  EXPECT_EQ(report.parity_pass, 1);
  ASSERT_EQ(report.parity_failures.size(), 1u);
  EXPECT_EQ(report.parity_failures[0].analysis, kA);
  EXPECT_EQ(report.parity_failures[0].age_difference, 0.0);
  EXPECT_NEAR(report.parity_failures[0].age_err_difference, 1.0 / 1001.0, 1e-12);

  // An age that is not a number never passes.
  report = check(batches, units_of(batches),
                 ages({{kA, ComputedAge{std::numeric_limits<double>::quiet_NaN(), 1e3}}, {kB, ComputedAge{1e-9, 0.0}}}));
  EXPECT_EQ(report.parity_fail, 1);
}

// The default tolerance is 1e-6, and the largest relative differences among
// the passing comparisons are reported, age and error apart.
TEST_P(VerifierTest, DefaultToleranceAndLargestPassingResidual) {
  const auto batches = history_with_age({{kA.str(), "66573-01", "100", "10"}, {kB.str(), "66573-02", "200", "20"}});
  run_import(batches);
  resolve(conflict_id(kUrl, "c4", "notes.txt"), "ignored");
  EXPECT_EQ(VerifyOptions{}.tolerance, 1e-6);

  // A: age off by 5e-7, error exact. B: age exact, error off by 2e-7. Both pass.
  auto report = check(batches, units_of(batches),
                      ages({{kA, ComputedAge{100 * (1 + 5e-7), 10.0}}, {kB, ComputedAge{200.0, 20 * (1 + 2e-7)}}}));
  EXPECT_EQ(report.parity_pass, 2);
  EXPECT_EQ(report.parity_fail, 0);
  EXPECT_NEAR(report.parity_max_pass_age_difference, 5e-7, 1e-10);
  EXPECT_NEAR(report.parity_max_pass_age_err_difference, 2e-7, 1e-10);

  // Off by 5e-6 fails; the failure does not count towards the maxima.
  report = check(batches, units_of(batches),
                 ages({{kA, ComputedAge{100 * (1 + 5e-6), 10.0}}, {kB, ComputedAge{200.0, 20.0}}}));
  EXPECT_EQ(report.parity_pass, 1);
  EXPECT_EQ(report.parity_fail, 1);
  EXPECT_EQ(report.parity_max_pass_age_difference, 0.0);
  EXPECT_EQ(report.parity_max_pass_age_err_difference, 0.0);
}

// The stored age was computed when the interpreted age was saved (c5). A's
// intercepts are refit afterwards (c6): the comparison is still as of c5.
// "As of" is a place in the walk, not a time: the later refit carries an
// author date before the interpreted age's (git dates run out of order).
TEST_P(VerifierTest, ParityIsAsOfTheInterpretedAge) {
  auto batches = history_with_age({{kA.str(), "66573-01", "28.25", "0.125"}});
  batches.push_back({});
  batches.back().changesets.push_back(refit("c6", kA, 1, 250.0, who("2016-03-06T12:00:00Z")));
  seal(batches);
  run_import(batches);
  resolve(conflict_id(kUrl, "c4", "notes.txt"), "ignored");

  // What an age function does: of each kind, the last revision whose source
  // commit is at or before the as-of commit in the walk order of the source.
  const std::vector<std::string> walk{"c1", "c2", "c3", "c4", "c5", "c6"};
  const auto place = [&](const std::string& commit) {
    return std::find(walk.begin(), walk.end(), commit) - walk.begin();
  };
  bool by_time_differs = false;
  const AgeFn fn = [&](Uuid analysis, const AsOf& as_of) -> Result<ParityAge> {
    EXPECT_EQ(as_of.source, source_uuid());
    EXPECT_EQ(as_of.commit, "c5");
    auto revisions = store().history(analysis, Kind::Intercepts);
    if (!revisions) return fail(revisions.error());
    std::optional<P::RevisionInfo> head, head_by_time;
    for (const auto& r : *revisions) {
      auto rows = store().provenance_for(r.uuid);
      if (!rows) return fail(rows.error());
      for (const auto& row : *rows)
        if (row.source == as_of.source && place(row.commit_sha) <= place(as_of.commit)) head = r;
      if (r.changeset.created <= as_of.created) head_by_time = r;
    }
    if (!head) return ParityAge{NotComparable{"no intercepts then"}};
    by_time_differs = head_by_time && head_by_time->uuid != head->uuid;
    const bool refit_again = head->uuid == revision_id(kUrl, "c6", kind_path("intercepts", 1));
    return ParityAge{ComputedAge{refit_again ? 99.0 : 28.25, 0.125}};
  };
  const auto report = check(batches, units_of(batches), fn);
  EXPECT_TRUE(report.ok());
  EXPECT_EQ(report.parity_pass, 1);
  EXPECT_EQ(report.parity_fail, 0);
  EXPECT_TRUE(by_time_differs) << "the test must tell walk order from time";
  // The head now is the later refit: reduced from the heads it would have failed.
  auto now = store().head(kA, Kind::Intercepts);
  ASSERT_TRUE(now && *now);
  EXPECT_EQ(**now, revision_id(kUrl, "c6", kind_path("intercepts", 1)));
}

// A member this source did not import cannot be placed in its walk: it is
// not comparable, and the age function is not asked.
TEST_P(VerifierTest, MemberOfAnotherSourceIsNotComparable) {
  {
    SourceDescription other = description();
    other.url = "https://github.com/NMGRLData/Blanks";
    std::vector<ImportBatch> theirs(1);
    theirs[0].catalog = lab_catalog();
    add_analysis(theirs[0], kC, 3, "x1", who("2016-03-01T00:00:00Z"));
    seal(theirs);
    FakeAdapter adapter(other, theirs);
    BatchWriter writer(store(), world_->client, config());
    ASSERT_TRUE(writer.run(adapter, std::nullopt, {}, {}));
  }
  auto batches = history_with_age({{kA.str(), "66573-01", "28.25", "0.125"}, {kC.str(), "66573-03", "30", "1"}});
  // C is also a member of this repository, by a copy of its record: still not this source's analysis.
  batches[2].memberships.push_back({kC, {"c4", "copy/73-03.json", "rec-3"}, who("2016-03-07T00:00:00Z"), {"Henry_Hill"}});
  run_import(batches);
  resolve(conflict_id(kUrl, "c4", "notes.txt"), "ignored");

  std::vector<Uuid> asked;
  const AgeFn fn = [&](Uuid analysis, const AsOf&) -> Result<ParityAge> {
    asked.push_back(analysis);
    return ParityAge{ComputedAge{28.25, 0.125}};
  };
  const auto report = check(batches, units_of(batches), fn);
  EXPECT_EQ(asked, std::vector<Uuid>{kA});
  EXPECT_EQ(report.parity_pass, 1);
  EXPECT_EQ(report.not_comparable_reasons, (std::map<std::string, int>{{"other_source", 1}}));
  EXPECT_TRUE(report.ok());
}

// A failure about a member the interpreted age no longer lists can never be
// compared again: its conflict is superseded, not left to block for good.
TEST_P(VerifierTest, ConflictOfADroppedMemberIsSuperseded) {
  auto batches = history_with_age({{kA.str(), "66573-01", "10", "1"}, {kB.str(), "66573-02", "20", "2"}});
  run_import(batches);
  resolve(conflict_id(kUrl, "c4", "notes.txt"), "ignored");
  const auto fn = ages({{kA, ComputedAge{10.0, 1.0}}, {kB, ComputedAge{25.0, 2.0}}});
  auto report = check(batches, units_of(batches), fn);
  EXPECT_EQ(report.parity_fail, 1);
  EXPECT_EQ(conflict(parity_conflict(kB)).resolution, "pending");
  // A value_mismatch of this source that is not a parity conflict is not touched.
  const Uuid other = conflict_id(kUrl, "c9", kAgePath);
  {
    auto uow = store().begin_import_batch(source_uuid(), world_->client);
    ASSERT_TRUE(uow);
    ASSERT_TRUE((*uow)->add_conflict({other, kAgePath, kB, P::ConflictKind::ValueMismatch, std::nullopt, std::nullopt,
                                      R"({"imported":true})", "pending"}));
    ASSERT_TRUE((*uow)->commit());
  }

  // The interpreted age is saved again without B.
  batches.push_back(age_batch("c6", "2017-01-01T00:00:00Z", {{kA.str(), "66573-01", "10", "1"}}));
  seal(batches);
  run_import(batches);
  report = check(batches, units_of(batches), fn);
  EXPECT_EQ(report.parity_pass, 1);
  EXPECT_EQ(report.parity_fail, 0);
  EXPECT_EQ(conflict(parity_conflict(kB)).resolution, "superseded");
  EXPECT_EQ(conflict(other).resolution, "pending");
  EXPECT_EQ(report.pending_blocking, 0);
  EXPECT_TRUE(report.ok());
}

// An interpreted age whose document lists no analyses is said so: it is not
// passed over in silence.
TEST_P(VerifierTest, InterpretedAgeWithoutAListIsTallied) {
  auto batches = history();
  batches.push_back(age_batch("c5", "2016-03-08T00:00:00Z", {}, R"({"name":"66573 plateau","age":28.2})"));
  seal(batches);
  run_import(batches);
  resolve(conflict_id(kUrl, "c4", "notes.txt"), "ignored");
  const auto report = check(batches, units_of(batches), ages({}));
  EXPECT_EQ(report.parity_pass + report.parity_pass_age_only + report.parity_fail, 0);
  EXPECT_EQ(report.not_comparable_reasons, (std::map<std::string, int>{{"interpreted age lists no analyses", 1}}));
  EXPECT_EQ(report.parity_not_comparable, 1);
}

// Every revision of an interpreted age is a statement of its time; only the
// head is compared.
TEST_P(VerifierTest, ParityComparesTheHeadRevisionOnly) {
  auto batches = history_with_age({{kA.str(), "66573-01", "10", "1"}, {kB.str(), "66573-02", "20", "2"}});
  batches.push_back(age_batch("c6", "2017-01-01T00:00:00Z", {{kA.str(), "66573-01", "11", "1"}}));
  seal(batches);
  run_import(batches);
  resolve(conflict_id(kUrl, "c4", "notes.txt"), "ignored");

  std::vector<AsOf> asked;
  const AgeFn fn = [&](Uuid, const AsOf& as_of) -> Result<ParityAge> {
    asked.push_back(as_of);
    return ParityAge{ComputedAge{11.0, 1.0}};
  };
  const auto report = check(batches, units_of(batches), fn);
  EXPECT_TRUE(report.ok());
  EXPECT_EQ(report.parity_pass, 1);  // A of the second revision; B is not a member of it
  EXPECT_EQ(report.parity_fail, 0);
  ASSERT_EQ(asked.size(), 1u);
  EXPECT_EQ(asked[0].changeset, changeset_id(kUrl, "c6"));
  EXPECT_EQ(asked[0].revision, revision_id(kUrl, "c6", kAgePath));
  EXPECT_EQ(asked[0].revision, **store().head(interpreted_age_id(kUrl, kAgePath), Kind::InterpretedAge));
  EXPECT_EQ(asked[0].commit, "c6");
  EXPECT_EQ(asked[0].created, *UtcTime::parse("2017-01-01T00:00:00Z"));
}

TEST_P(VerifierTest, PassingReverifySupersedesTheParityConflict) {
  const auto batches = history_with_age({{kA.str(), "66573-01", "28.25", "0.125"}});
  run_import(batches);
  resolve(conflict_id(kUrl, "c4", "notes.txt"), "ignored");
  const auto units = units_of(batches);
  const Uuid id = parity_conflict(kA);
  const auto wrong = ages({{kA, ComputedAge{30.0, 0.125}}});
  const auto right = ages({{kA, ComputedAge{28.25, 0.125}}});

  auto report = check(batches, units, wrong);
  EXPECT_FALSE(report.ok());
  EXPECT_EQ(conflict(id).resolution, "pending");
  const auto rows = world_->counts();

  // Failing again adds nothing.
  report = check(batches, units, wrong);
  EXPECT_EQ(report.parity_fail, 1);
  EXPECT_EQ(report.pending_blocking, 1);
  EXPECT_EQ(world_->counts(), rows);

  // Not comparable is neither a pass nor a failure: the conflict stays.
  report = check(batches, units, ages({{kA, NotComparable{"no J"}}}));
  EXPECT_EQ(report.parity_not_comparable, 1);
  EXPECT_EQ(conflict(id).resolution, "pending");
  EXPECT_FALSE(report.ok());

  // Passing: superseded, and verify is ok.
  report = check(batches, units, right);
  EXPECT_TRUE(report.ok());
  EXPECT_EQ(report.parity_pass, 1);
  EXPECT_EQ(report.pending_blocking, 0);
  EXPECT_EQ(conflict(id).resolution, "superseded");
  EXPECT_EQ(world_->counts(), rows);

  // Failing after that: pending again, still one row.
  report = check(batches, units, wrong);
  EXPECT_FALSE(report.ok());
  EXPECT_EQ(conflict(id).resolution, "pending");
  EXPECT_EQ(world_->counts(), rows);

  // A conflict someone resolved by hand is left as it is.
  resolve(id, "accepted");
  report = check(batches, units, wrong);
  EXPECT_EQ(report.parity_fail, 1);
  EXPECT_EQ(conflict(id).resolution, "accepted");
  report = check(batches, units, right);
  EXPECT_EQ(conflict(id).resolution, "accepted");
}

TEST_P(VerifierTest, MembersThatCannotBeComparedAreTallied) {
  const auto batches = history_with_age({{"", "66573-09", "28.0", "0.1"},           // no uuid
                                         {kA.str(), "66573-01"},                     // no legacy age
                                         {kAbsent.str(), "66573-03", "28.75", "0.5"},  // not imported
                                         {kB.str(), "66573-02", "28.5"},             // an age without an error
                                         {kB.str(), "66573-02", "28.5"}});           // listed twice
  run_import(batches);
  resolve(conflict_id(kUrl, "c4", "notes.txt"), "ignored");
  const auto units = units_of(batches);

  // B has no legacy error: the age alone is compared, and counted apart from a full pass.
  auto report = check(batches, units, ages({{kB, ComputedAge{28.5, 7.0}}}));
  EXPECT_TRUE(report.ok()) << "not comparable is not a failure";
  EXPECT_EQ(report.parity_pass, 0);
  EXPECT_EQ(report.parity_pass_age_only, 1);
  EXPECT_EQ(report.parity_fail, 0);
  EXPECT_EQ(report.parity_not_comparable, 3);
  EXPECT_EQ(report.not_comparable_reasons,
            (std::map<std::string, int>{
                {"member has no uuid", 1}, {"no legacy age", 1}, {"analysis is not in the store", 1}}));

  // The function's own reasons are kept and tallied; an empty one has a name.
  const AgeFn reasons = [](Uuid, const AsOf&) -> Result<ParityAge> { return ParityAge{NotComparable{"no J"}}; };
  report = check(batches, units, reasons);
  EXPECT_EQ(report.parity_pass, 0);
  EXPECT_EQ(report.parity_not_comparable, 4);
  EXPECT_EQ(report.not_comparable_reasons.at("no J"), 1);

  // Without a function nothing is compared, and nothing passes.
  report = check(batches, units);
  EXPECT_EQ(report.parity_pass, 0);
  EXPECT_EQ(report.parity_not_comparable, 4);
  EXPECT_EQ(report.not_comparable_reasons.at("no age function"), 1);
  EXPECT_TRUE(report.ok());
}

TEST_P(VerifierTest, AgeFunctionErrorIsReturned) {
  const auto batches = history_with_age({{kA.str(), "66573-01", "28.25", "0.125"}});
  run_import(batches);
  FakeAdapter adapter(description(), batches);
  adapter.honour_token(true);
  adapter.units(units_of(batches));
  const AgeFn broken = [](Uuid, const AsOf&) -> Result<ParityAge> { return fail(ErrorKind::Io, "reduction: boom"); };
  auto report = verify(store(), world_->client, adapter, config(), broken, {});
  ASSERT_FALSE(report);
  EXPECT_NE(report.error().what.find("boom"), std::string::npos);
}

// A legacy error includes the error of J or not. The member's own
// age_err_wo_j is compared with the computed error without J, whatever the
// file says about age_err.
TEST_P(VerifierTest, ParityComparesTheErrorWithoutJWhenTheMemberHasOne) {
  const std::vector<Member> members{{kA.str(), "66573-01", "28.25", "0.5", "0.125"}};
  const auto batches = history_with_age(members, age_document(members, "true"));
  run_import(batches);
  resolve(conflict_id(kUrl, "c4", "notes.txt"), "ignored");
  const auto units = units_of(batches);

  // age_err (0.5, with J) is not what is compared: a computed 9.0 with J does not matter.
  auto report = check(batches, units, ages({{kA, ComputedAge{28.25, 0.125, 9.0}}}));
  EXPECT_EQ(report.parity_pass, 1);
  EXPECT_EQ(report.parity_fail, 0);

  report = check(batches, units, ages({{kA, ComputedAge{28.25, 0.25, 0.5, "constants=test"}}}));
  ASSERT_EQ(report.parity_failures.size(), 1u);
  EXPECT_EQ(report.parity_failures[0].error_compared, "age_err_wo_j");
  EXPECT_EQ(report.parity_failures[0].legacy_age_err, 0.125);
  EXPECT_EQ(report.parity_failures[0].computed_age_err, 0.25);
  EXPECT_EQ(report.parity_failures[0].basis, "constants=test");
  const std::string detail = conflict(parity_conflict(kA)).detail_json;
  for (const char* part : {R"("error_compared":"age_err_wo_j")", R"("basis":"constants=test")",
                           R"("legacy":{"age":28.25,"age_err":0.125})", R"("computed":{"age":28.25,"age_err":0.25})"})
    EXPECT_NE(detail.find(part), std::string::npos) << part << " in " << detail;
}

// Without age_err_wo_j the file's flag says which computed error age_err is
// compared with.
TEST_P(VerifierTest, ParityComparesAgeErrAsTheFileSaysItWasComputed) {
  const std::vector<Member> members{{kA.str(), "66573-01", "28.25", "0.5"}};
  for (const bool with_j : {true, false}) {
    SCOPED_TRACE(with_j);
    SetUp();  // a fresh store for each file
    const auto batches = history_with_age(members, age_document(members, with_j ? "true" : "false"));
    run_import(batches);
    resolve(conflict_id(kUrl, "c4", "notes.txt"), "ignored");
    const auto units = units_of(batches);

    const ComputedAge matches = with_j ? ComputedAge{28.25, 0.125, 0.5} : ComputedAge{28.25, 0.5, 0.75};
    auto report = check(batches, units, ages({{kA, matches}}));
    EXPECT_EQ(report.parity_pass, 1);
    EXPECT_EQ(report.parity_fail, 0);

    // The other error holding the legacy value is not a match.
    const ComputedAge swapped = with_j ? ComputedAge{28.25, 0.5, 0.125} : ComputedAge{28.25, 0.75, 0.5};
    report = check(batches, units, ages({{kA, swapped}}));
    ASSERT_EQ(report.parity_failures.size(), 1u);
    EXPECT_EQ(report.parity_failures[0].error_compared, with_j ? "age_err_w_j" : "age_err");
    EXPECT_EQ(report.parity_failures[0].legacy_age_err, 0.5);
    EXPECT_EQ(report.parity_failures[0].computed_age_err, with_j ? 0.125 : 0.75);
  }
}

// Neither age_err_wo_j nor the flag: nothing says what age_err holds, so the
// age alone is compared. The same when the file's errors include J and the
// age function computed none with J.
TEST_P(VerifierTest, ParityComparesTheAgeAloneWhenTheKindOfErrorIsUnknown) {
  const std::vector<Member> members{{kA.str(), "66573-01", "28.25", "0.5"}};
  auto batches = history_with_age(members, age_document(members, ""));
  run_import(batches);
  resolve(conflict_id(kUrl, "c4", "notes.txt"), "ignored");
  auto report = check(batches, units_of(batches), ages({{kA, ComputedAge{28.25, 7.0, 9.0}}}));
  EXPECT_TRUE(report.ok());
  EXPECT_EQ(report.parity_pass, 0);
  EXPECT_EQ(report.parity_pass_age_only, 1);
  EXPECT_EQ(report.parity_fail, 0);

  // The age itself still has to agree, and the failure names no error.
  report = check(batches, units_of(batches), ages({{kA, ComputedAge{30.0, 0.5, 0.5}}}));
  ASSERT_EQ(report.parity_failures.size(), 1u);
  EXPECT_EQ(report.parity_failures[0].error_compared, "");
  EXPECT_FALSE(report.parity_failures[0].legacy_age_err);

  SetUp();
  batches = history_with_age(members, age_document(members, "true"));
  run_import(batches);
  resolve(conflict_id(kUrl, "c4", "notes.txt"), "ignored");
  report = check(batches, units_of(batches), ages({{kA, ComputedAge{28.25, 7.0}}}));
  EXPECT_EQ(report.parity_pass_age_only, 1);
  EXPECT_EQ(report.parity_fail, 0);
}

// An interpreted age that was not imported has nothing to compare: the
// accounting reports its file.
TEST_P(VerifierTest, InterpretedAgeThatIsNotImportedIsNotCompared) {
  const auto batches = history();
  run_import(batches);
  resolve(conflict_id(kUrl, "c4", "notes.txt"), "ignored");
  auto units = units_of(batches);
  const SourceKey key{"c5", kAgePath, "ia-c5"};
  units.push_back(unit(key, UnitDisposition::Imported, {recorded(key)}));
  units.back().interpreted_age = kAgePath;
  bool asked = false;
  const AgeFn fn = [&](Uuid, const AsOf&) -> Result<ParityAge> {
    asked = true;
    return ParityAge{ComputedAge{}};
  };
  const auto report = check(batches, units, fn);
  EXPECT_FALSE(asked);
  ASSERT_EQ(report.unaccounted.size(), 1u);
  EXPECT_EQ(report.unaccounted[0].unit.path, kAgePath);
  EXPECT_EQ(report.parity_pass + report.parity_fail + report.parity_not_comparable, 0);
}

INSTANTIATE_TEST_SUITE_P(Engines, VerifierTest, ::testing::ValuesIn(P::testing::engines()),
                         [](const auto& p) { return p.param; });
