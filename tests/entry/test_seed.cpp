// The seed file (install defaults design, 2026-10-07): read, checked, and
// put in a store without changing anything that is there.

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <sstream>

#include "pychron/entry/package_edit.hpp"
#include "pychron/entry/seed.hpp"
#include "store_fixture.hpp"

using namespace pychron;
using namespace pychron::entry;
using namespace pychron::persistence;
using namespace pychron::persistence::testing;

namespace {

constexpr const char* kSmall = R"(
project = "references"

[[samples]]
identifier = "bu"
analysis_type = "blank_unknown"
sample = "blank_unknown"
material = "blank"

[[samples]]
identifier = "a"
analysis_type = "air"
sample = "air"
material = "air"

[reactors.Triga]
Cl_K = [0.227, 0.0]
K4039 = [0.00873, 0.00017]
)";

void expect_refused(const std::string& text, const std::string& offender) {
  auto s = parse_seed(text, "my-seed.toml");
  ASSERT_FALSE(s) << text;
  EXPECT_EQ(s.error().kind, ErrorKind::Config);
  EXPECT_NE(s.error().what.find("my-seed.toml"), std::string::npos) << s.error().what;
  EXPECT_NE(s.error().what.find(offender), std::string::npos) << s.error().what;
}

std::string sample(const std::string& body) { return "project = \"references\"\n[[samples]]\n" + body; }

}  // namespace

TEST(Seed, Parses) {
  auto s = parse_seed(kSmall);
  ASSERT_TRUE(s) << s.error().what;
  EXPECT_EQ(s->project, "references");
  EXPECT_EQ(s->samples, (std::vector<SeedSample>{{"bu", "blank_unknown", "blank_unknown", "blank"}, {"a", "air", "air", "air"}}));
  ASSERT_EQ(s->reactors.size(), 1u);
  const auto& triga = s->reactors.at("Triga");
  EXPECT_EQ(triga.reactor, std::optional<std::string>("Triga"));
  // In production_keys() order, whatever the file's.
  EXPECT_EQ(triga.ratios, (std::vector<ProductionRatio>{{"K4039", 0.00873, 0.00017}, {"Cl_K", 0.227, 0.0}}));
}

TEST(Seed, AFileWithNoSamplesAndNoReactorsIsASeedOfTheProjectAlone) {
  auto s = parse_seed("project = \"references\"\n");
  ASSERT_TRUE(s) << s.error().what;
  EXPECT_TRUE(s->samples.empty());
  EXPECT_TRUE(s->reactors.empty());
}

TEST(Seed, RefusesWhatIsNotToml) { expect_refused("project = \n", "syntax"); }

TEST(Seed, RefusesUnknownKeys) {
  expect_refused("project = \"references\"\nprojects = 1\n", "'projects'");
  expect_refused(sample("identifier = \"a\"\nanalysis_type = \"air\"\nsample = \"air\"\nmaterial = \"air\"\nnote = \"x\"\n"),
                 "'note'");
  expect_refused("project = \"references\"\n[reactors.Triga]\nK4038 = [1.0, 0.0]\n", "'K4038'");
}

TEST(Seed, RefusesAMissingOrMalformedProject) {
  expect_refused("[[samples]]\n", "project");
  expect_refused("project = \"my refs\"\n", "'my refs'");
  expect_refused("project = 3\n", "project");
}

TEST(Seed, RefusesASampleWithoutAllItsFields) {
  expect_refused(sample("analysis_type = \"air\"\nsample = \"air\"\nmaterial = \"air\"\n"), "identifier");
  expect_refused(sample("identifier = \"a\"\nsample = \"air\"\nmaterial = \"air\"\n"), "analysis_type");
  expect_refused(sample("identifier = \"a\"\nanalysis_type = \"air\"\nmaterial = \"air\"\n"), "sample");
  expect_refused(sample("identifier = \"a\"\nanalysis_type = \"air\"\nsample = \"air\"\n"), "material");
  expect_refused(sample("identifier = \"a\"\nanalysis_type = \"air\"\nsample = \"\"\nmaterial = \"air\"\n"), "sample");
}

TEST(Seed, RefusesAnAnalysisTypeThatHasNoReferenceSample) {
  for (const char* type : {"blank", "unknown", "pause", "degas"})
    expect_refused(sample(std::string("identifier = \"x\"\nanalysis_type = \"") + type + "\"\nsample = \"s\"\nmaterial = \"m\"\n"),
                   std::string("'") + type + "'");
}

TEST(Seed, RefusesAnIdentifierGivenTwice) {
  const std::string one = "identifier = \"a\"\nanalysis_type = \"air\"\nsample = \"air\"\nmaterial = \"air\"\n";
  expect_refused("project = \"references\"\n[[samples]]\n" + one + "[[samples]]\nidentifier = \"A\"\nanalysis_type = \"air\"\nsample = \"air2\"\nmaterial = \"air\"\n",
                 "'A'");
}

TEST(Seed, RefusesARatioThatIsNotTwoFiniteNumbers) {
  const std::string head = "project = \"references\"\n[reactors.Triga]\n";
  expect_refused(head + "K4039 = 0.1\n", "K4039");
  expect_refused(head + "K4039 = [0.1]\n", "K4039");
  expect_refused(head + "K4039 = [0.1, \"x\"]\n", "K4039");
  expect_refused(head + "K4039 = [nan, 0.0]\n", "K4039");
  expect_refused(head + "K4039 = [0.1, inf]\n", "K4039");
  expect_refused("project = \"references\"\nreactors = 3\n", "reactors");
  expect_refused("project = \"references\"\n[reactors]\nTriga = 3\n", "Triga");
}

TEST(Seed, IntegersAreNumbers) {
  auto s = parse_seed("project = \"references\"\n[reactors.Triga]\nCl3638 = [250, 0]\n");
  ASSERT_TRUE(s) << s.error().what;
  EXPECT_EQ(s->reactors.at("Triga").ratios, (std::vector<ProductionRatio>{{"Cl3638", 250.0, 0.0}}));
}

// ---------------------------------------------------------------- in a store

namespace {

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// The seed an instrument install ships.
Seed shipped() {
  auto s = parse_seed(read_file(std::string(PYCHRON_PROFILES_DIR) + "/instrument-common/seed.toml"));
  EXPECT_TRUE(s) << (s ? "" : s.error().what);
  return s ? *s : Seed{};
}

bool has(const std::vector<std::string>& kept, const std::string& what) {
  return std::find(kept.begin(), kept.end(), what) != kept.end();
}

class SeedStore : public StoreTest {
 protected:
  Actor actor() const { return Actor{lab_.reducer, lab_.reduction_client}; }

  std::optional<ProjectRow> project(const std::string& name) {
    auto rows = store_->projects(std::nullopt);
    EXPECT_TRUE(rows);
    for (const auto& r : *rows)
      if (r.name == name) return r;
    return std::nullopt;
  }
  std::vector<SampleRow> samples_of(const std::string& project_name) {
    auto p = project(project_name);
    if (!p) return {};
    SampleQuery q;
    q.project = p->uuid;
    auto rows = store_->samples(q);
    EXPECT_TRUE(rows);
    return rows ? *rows : std::vector<SampleRow>{};
  }
  // Whether something (here: an identifier) is of this sample: the store
  // refuses to delete a sample that is in use, and deletes one that is not.
  bool in_use(const SampleRow& sample) {
    auto fields = store_->catalog_row(CatalogTable::Sample, sample.uuid);
    EXPECT_TRUE(fields && *fields);
    CatalogEditBatch batch;
    batch.edits.emplace_back(CatalogDelete{CatalogTable::Sample, sample.uuid, **fields});
    auto outcome = store_->apply_catalog_edits(actor().client, batch);
    EXPECT_TRUE(outcome) << (outcome ? "" : outcome.error().what);
    return outcome && std::holds_alternative<std::vector<Refusal>>(*outcome);
  }
  std::optional<Uuid> reactors_object() {
    auto o = store_->find_catalog_row(CatalogTable::RefObject, {std::string("document"), std::string("reactors.json")});
    EXPECT_TRUE(o);
    return o ? *o : std::nullopt;
  }
  std::size_t reactors_revisions() {
    auto o = reactors_object();
    if (!o) return 0;
    auto h = store_->history(*o, Kind::RefValue);
    EXPECT_TRUE(h);
    return h ? h->size() : 0;
  }
  void write_reactors(DocumentValue doc) {
    RefObjectSpec spec;
    spec.type = RefType::Document;
    spec.key = "reactors.json";
    auto object = store_->add_ref_object(actor().client, spec);
    ASSERT_TRUE(object) << object.error().what;
    auto uow = store_->begin(actor());
    ASSERT_TRUE(uow);
    ASSERT_TRUE((*uow)->add_revision(*object, Kind::RefValue, RevisionPayload{RefPayload{doc}}, std::nullopt));
    ASSERT_TRUE((*uow)->commit(ChangesetKind::Reference, "reactors"));
  }
  Seed with_osu() {
    Seed seed = shipped();
    ProductionValue osu;
    osu.reactor = "Osu";
    osu.ratios = {{"K4039", 0.0007, 0.0001}};
    seed.reactors.emplace("Osu", osu);
    return seed;
  }
};

}  // namespace

TEST_P(SeedStore, AnEmptyStoreGetsEverything) {
  auto report = apply_seed(*store_, shipped(), actor());
  ASSERT_TRUE(report) << report.error().what;
  EXPECT_EQ(report->projects, 1);
  EXPECT_EQ(report->materials, 3);
  EXPECT_EQ(report->samples, 8);
  EXPECT_EQ(report->identifiers, 8);
  EXPECT_EQ(report->reactors, 1);
  EXPECT_TRUE(report->kept.empty());
  EXPECT_EQ(describe(*report), "seeded 1 project, 3 materials, 8 samples, 8 identifiers, 1 reactor");

  const auto p = project("references");
  ASSERT_TRUE(p);
  EXPECT_FALSE(p->principal_investigator);
  const auto samples = samples_of("references");
  ASSERT_EQ(samples.size(), 8u);
  for (const auto& s : samples) {
    EXPECT_TRUE(store_->find_identifier(s.name == "air" ? "a" : "bu").value().has_value());
    EXPECT_TRUE(in_use(s)) << s.name << " has no identifier";
    if (s.name == "air" || s.name == "detector_ic") EXPECT_EQ(s.material_name, "air");
    if (s.name == "background") EXPECT_EQ(s.material_name, "blank");
  }
  for (const char* id : {"bu", "ba", "bc", "be", "bg", "a", "c", "ic"})
    EXPECT_TRUE(store_->find_identifier(id).value().has_value()) << id;

  auto reactors = load_reactors(*store_);
  ASSERT_TRUE(reactors) << reactors.error().what;
  ASSERT_EQ(reactors->size(), 1u);
  EXPECT_EQ(reactors->at("Triga").ratios,
            (std::vector<ProductionRatio>{{"K4039", 0.00873, 0.00017}, {"K3839", 0.013, 0.0},     {"K3739", 0.0, 0.0},
                                          {"Ca3937", 0.000758, 7e-06}, {"Ca3837", 4e-05, 2e-05},  {"Ca3637", 0.000286, 5e-07},
                                          {"Cl3638", 250.0, 0.0},      {"Ca_K", 1.96, 0.0},       {"Cl_K", 0.227, 0.0}}));
}

TEST_P(SeedStore, ASecondRunWritesNothing) {
  ASSERT_TRUE(apply_seed(*store_, shipped(), actor()));
  const auto revisions = reactors_revisions();
  auto again = apply_seed(*store_, shipped(), actor());
  ASSERT_TRUE(again) << again.error().what;
  EXPECT_FALSE(again->changed());
  EXPECT_EQ(again->kept.size(), 21u);  // 1 project, 3 materials, 8 samples, 8 identifiers, 1 reactor
  EXPECT_EQ(describe(*again), "seed: nothing to add (21 already there)");
  EXPECT_EQ(reactors_revisions(), revisions);
  EXPECT_EQ(samples_of("references").size(), 8u);
  EXPECT_EQ(store_->materials().value().size(), 3u);
}

TEST_P(SeedStore, ADryRunReportsAndWritesNothing) {
  auto report = apply_seed(*store_, shipped(), actor(), true);
  ASSERT_TRUE(report) << report.error().what;
  EXPECT_EQ(describe(*report), "seeded 1 project, 3 materials, 8 samples, 8 identifiers, 1 reactor");
  EXPECT_FALSE(project("references"));
  EXPECT_TRUE(store_->materials().value().empty());
  EXPECT_FALSE(store_->find_identifier("bu").value().has_value());
  EXPECT_FALSE(reactors_object());
  // And on a seeded store it finds everything there.
  ASSERT_TRUE(apply_seed(*store_, shipped(), actor()));
  auto again = apply_seed(*store_, shipped(), actor(), true);
  ASSERT_TRUE(again);
  EXPECT_FALSE(again->changed());
}

// Review focus 1: a lab's own references project, with its principal
// investigator, and a sample it already has there.
TEST_P(SeedStore, WhatIsThereIsKept) {
  const auto client = actor().client;
  auto pi = store_->add_principal_investigator(client, {"Ross", "J"});
  ASSERT_TRUE(pi) << pi.error().what;
  ProjectSpec ps;
  ps.name = "references";
  ps.principal_investigator = *pi;
  auto proj = store_->add_project(client, ps);
  ASSERT_TRUE(proj);
  auto air = store_->add_material(client, {"air"});
  ASSERT_TRUE(air);
  SampleSpec ss;
  ss.name = "air";
  ss.project = *proj;
  ss.material = *air;
  ss.note = "the lab's own";
  ASSERT_TRUE(store_->add_sample(client, ss));

  auto report = apply_seed(*store_, shipped(), actor());
  ASSERT_TRUE(report) << report.error().what;
  EXPECT_EQ(report->projects, 0);
  EXPECT_EQ(report->materials, 2);
  EXPECT_EQ(report->samples, 7);
  EXPECT_EQ(report->identifiers, 8);
  EXPECT_TRUE(has(report->kept, "project references"));
  EXPECT_TRUE(has(report->kept, "material air"));
  EXPECT_TRUE(has(report->kept, "sample air"));

  auto projects = store_->projects(std::nullopt);
  ASSERT_TRUE(projects);
  EXPECT_EQ(std::count_if(projects->begin(), projects->end(), [](const ProjectRow& r) { return r.name == "references"; }), 1);
  EXPECT_EQ(project("references")->principal_investigator, std::optional<Uuid>(*pi));
  const auto samples = samples_of("references");
  EXPECT_EQ(samples.size(), 8u);
  for (const auto& s : samples)
    if (s.name == "air") EXPECT_EQ(s.fields.note, std::optional<std::string>("the lab's own"));
}

// Review focus 2: a special identifier the legacy importer made has no sample.
TEST_P(SeedStore, AnIdentifierThatIsThereIsKeptAsItIs) {
  IdentifierSpec spec;
  spec.identifier = "bu";
  spec.kind = "special";
  spec.analysis_type = "blank_unknown";
  ASSERT_TRUE(store_->add_identifier(actor().client, spec));

  auto report = apply_seed(*store_, shipped(), actor());
  ASSERT_TRUE(report) << report.error().what;
  EXPECT_EQ(report->identifiers, 7);
  EXPECT_EQ(report->samples, 8);
  EXPECT_TRUE(has(report->kept, "identifier bu"));
  for (const auto& s : samples_of("references"))
    if (s.name == "blank_unknown") EXPECT_FALSE(in_use(s)) << "the identifier was given a sample";
}

TEST_P(SeedStore, AnEditedReactorIsNotChangedAndAMissingOneIsAdded) {
  DocumentValue doc;
  doc.content_json = R"({"Triga": {"K4039": [0.01, 0.001], "source_path": "x"}, "note": "ours"})";
  write_reactors(doc);
  ASSERT_EQ(reactors_revisions(), 1u);

  auto report = apply_seed(*store_, with_osu(), actor());
  ASSERT_TRUE(report) << report.error().what;
  EXPECT_EQ(report->reactors, 1);
  EXPECT_TRUE(has(report->kept, "reactor Triga"));
  EXPECT_EQ(reactors_revisions(), 2u);
  auto reactors = load_reactors(*store_);
  ASSERT_TRUE(reactors) << reactors.error().what;
  EXPECT_EQ(reactors->at("Triga").ratios, (std::vector<ProductionRatio>{{"K4039", 0.01, 0.001}}));
  EXPECT_EQ(reactors->at("Osu").ratios, (std::vector<ProductionRatio>{{"K4039", 0.0007, 0.0001}}));
  // What the document held beside the ratios is still in it.
  auto payload = store_->load_payload(*store_->head(*reactors_object(), Kind::RefValue).value());
  ASSERT_TRUE(payload && *payload);
  const auto& now = std::get<DocumentValue>(std::get<RefPayload>(**payload));
  ASSERT_TRUE(now.content_json);
  EXPECT_NE(now.content_json->find("source_path"), std::string::npos);
  EXPECT_NE(now.content_json->find("ours"), std::string::npos);

  // Nothing more to add: no third revision.
  ASSERT_TRUE(apply_seed(*store_, with_osu(), actor()));
  EXPECT_EQ(reactors_revisions(), 2u);
}

// Review focus 3.
TEST_P(SeedStore, AReactorsDocumentKeptAsTextIsReadAndExtended) {
  DocumentValue doc;
  doc.content_text = R"({"Triga": {"K4039": [0.01, 0.001]}})";
  write_reactors(doc);
  auto report = apply_seed(*store_, with_osu(), actor());
  ASSERT_TRUE(report) << report.error().what;
  EXPECT_EQ(report->reactors, 1);
  auto reactors = load_reactors(*store_);
  ASSERT_TRUE(reactors) << reactors.error().what;
  EXPECT_EQ(reactors->at("Triga").ratios, (std::vector<ProductionRatio>{{"K4039", 0.01, 0.001}}));
  EXPECT_EQ(reactors->count("Osu"), 1u);
}

TEST_P(SeedStore, AReactorsDocumentThatIsNotJsonIsLeftAloneAndSaid) {
  DocumentValue doc;
  doc.content_text = "not json";
  write_reactors(doc);
  const auto head = store_->head(*reactors_object(), Kind::RefValue).value();
  for (bool dry : {true, false}) {
    auto report = apply_seed(*store_, shipped(), actor(), dry);
    ASSERT_FALSE(report);
    EXPECT_EQ(report.error().kind, ErrorKind::Config);
    EXPECT_NE(report.error().what.find("reactors.json"), std::string::npos) << report.error().what;
    EXPECT_EQ(store_->head(*reactors_object(), Kind::RefValue).value(), head);
  }
  // The reactors come last: the rows are there.
  EXPECT_EQ(samples_of("references").size(), 8u);
}

INSTANTIATE_TEST_SUITE_P(Engines, SeedStore, ::testing::ValuesIn(engines()),
                         [](const auto& param_info) { return param_info.param; });
