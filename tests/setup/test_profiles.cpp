// Every shipped profile installs with its default answers (instruments in
// simulation and on hardware), the result loads with the apps' own loaders,
// its example queue checks against the lab, and doctor finds nothing to fail.

#include <gtest/gtest.h>

#include <random>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <toml++/toml.hpp>

#include "pychron/experiment/factory/defaults.hpp"
#include "pychron/experiment/model/identifiers.hpp"

#include "pychron/experiment/lab/lab.hpp"
#include "pychron/experiment/model/queue_file.hpp"
#include "pychron/setup/doctor.hpp"
#include "pychron/setup/install.hpp"
#include "pychron/setup/installer.hpp"
#include "pychron/setup/profile.hpp"

using namespace pychron;
using namespace pychron::setup;
namespace fs = std::filesystem;

namespace {

const fs::path kProfiles(PYCHRON_PROFILES_DIR);
const fs::path kExamples(PYCHRON_EXAMPLE_CONFIGS_DIR);

fs::path scratch(const std::string& tag) {
  const fs::path dir = fs::temp_directory_path() / ("pychron-setup-" + tag + "-" +
                                                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                                                     std::to_string(std::random_device{}()));
  fs::remove_all(dir);
  return dir;
}

ProfileLibrary library() {
  auto lib = ProfileLibrary::load(kProfiles, kExamples);
  EXPECT_TRUE(lib) << (lib ? "" : lib.error().what);
  return std::move(*lib);
}

// Installs `name` into a scratch root; returns the site entry.
SiteInstall install(const ProfileLibrary& lib, const std::string& name, const Answers& given, const fs::path& root) {
  auto resolved = lib.resolve(name);
  EXPECT_TRUE(resolved) << (resolved ? "" : resolved.error().what);
  const Answers builtins{{"install_name", Value{name}}, {"root", Value{root.generic_string()}}};
  auto answers = complete_answers(*resolved, given, builtins);
  EXPECT_TRUE(answers) << (answers ? "" : answers.error().what);
  auto plan = plan_install(lib, *resolved, *answers, root);
  EXPECT_TRUE(plan) << (plan ? "" : plan.error().what);
  auto report = apply_install(*plan);
  EXPECT_TRUE(report) << (report ? "" : report.error().what);
  return site_install(*plan, name);
}

void expect_no_failures(const SiteInstall& site) {
  for (const auto& c : doctor(site)) {
    EXPECT_NE(c.status, Check::Status::Fail) << site.profile << ": " << c.name << ": " << c.detail;
  }
}

void expect_queue_checks(const SiteInstall& site) {
  auto lab = experiment::lab::load_lab({site.root, site.path(site.line), site.path(site.spectrometer)});
  ASSERT_TRUE(lab.problems.empty()) << lab.problems.front();
  auto queue = experiment::load_queue_file((site.root / "experiment.toml").string(), lab.ids);
  ASSERT_TRUE(queue) << queue.error().what;
  const auto check = experiment::lab::check_lab_queue(lab, *queue);
  for (const auto& d : check.all())
    EXPECT_NE(d.severity, experiment::Severity::Error) << site.profile << ": " << experiment::lab::describe(d);
}

}  // namespace

TEST(Profiles, TheLibraryLoadsAndEveryProfileResolves) {
  const auto lib = library();
  std::vector<std::string> installable;
  for (const auto* p : lib.list()) {
    auto r = lib.resolve(p->name);
    EXPECT_TRUE(r) << p->name << ": " << (r ? "" : r.error().what);
    if (p->kind != ProfileKind::Fragment) installable.push_back(p->name);
  }
  EXPECT_EQ(installable, (std::vector<std::string>{"argus", "data-reduction", "helix", "ngx"}));
}

class InstrumentProfile : public ::testing::TestWithParam<std::tuple<std::string, bool>> {};

TEST_P(InstrumentProfile, InstallsLoadsAndPassesDoctor) {
  const auto& [name, simulation] = GetParam();
  const auto lib = library();
  const fs::path root = scratch(name);
  const auto site = install(lib, name, {{"simulation", Value{simulation}}}, root);
  EXPECT_EQ(site.kind, "instrument");
  EXPECT_EQ(site.simulation, simulation);
  for (const char* f : {"extraction_line.toml", "canvas.toml", "spectrometer.toml", "plans/multicollect.toml",
                        "plans/detector_ic.toml", "plans/multicollect_hop_ar39.toml", "peak_center.toml", "experiment.toml", "CALIBRATE.md", "scripts/extraction/sim_extract.py"})
    EXPECT_TRUE(fs::exists(root / f)) << name << ": " << f;
  expect_no_failures(site);
  expect_queue_checks(site);
  fs::remove_all(root);
}

// The eight kinds of run that are of a reference sample, as seed.toml has them.
struct ReferenceRow {
  const char* identifier;
  experiment::AnalysisType type;
  const char* sample;
  const char* material;
};
const std::vector<ReferenceRow> kReferences{
    {"bu", experiment::AnalysisType::BlankUnknown, "blank_unknown", "blank"},
    {"ba", experiment::AnalysisType::BlankAir, "blank_air", "blank"},
    {"bc", experiment::AnalysisType::BlankCocktail, "blank_cocktail", "blank"},
    {"be", experiment::AnalysisType::BlankExtractionLine, "blank_extractionline", "blank"},
    {"bg", experiment::AnalysisType::Background, "background", "blank"},
    {"a", experiment::AnalysisType::Air, "air", "air"},
    {"c", experiment::AnalysisType::Cocktail, "cocktail", "cocktail"},
    {"ic", experiment::AnalysisType::DetectorIC, "detector_ic", "air"},
};

TEST_P(InstrumentProfile, ShipsIdentifiersThatAreTheBuiltInOnes) {
  const auto& [name, simulation] = GetParam();
  const fs::path root = scratch(name);
  install(library(), name, {{"simulation", Value{simulation}}}, root);
  auto shipped = experiment::IdentifierRules::load((root / "identifiers.toml").string());
  ASSERT_TRUE(shipped) << shipped.error().what;
  const auto built_in = experiment::IdentifierRules::defaults();
  using T = experiment::AnalysisType;
  for (T t : {T::BlankUnknown, T::BlankAir, T::BlankCocktail, T::BlankExtractionLine, T::Background, T::Air,
              T::Cocktail, T::Pause, T::Degas, T::DetectorIC}) {
    EXPECT_EQ(shipped->prefix_for(t), built_in.prefix_for(t)) << experiment::to_string(t);
    EXPECT_EQ(shipped->classify(built_in.prefix_for(t)), t);
  }
  for (const char* id : {"u", "b", "bu", "ba", "bc", "be", "bg", "a", "c", "pa", "dg", "ic", "66001", "bu-2", "x y"}) {
    EXPECT_EQ(shipped->classify(id), built_in.classify(id)) << id;
    EXPECT_EQ(shipped->validate_identifier(id).has_value(), built_in.validate_identifier(id).has_value()) << id;
  }
  for (const char* step : {"", "A", "ab", "abc", "1"}) EXPECT_EQ(shipped->valid_step(step), built_in.valid_step(step)) << step;
  fs::remove_all(root);
}

TEST_P(InstrumentProfile, ShipsASeedThatTheDefaultsAndTheExampleQueueAgreeWith) {
  const auto& [name, simulation] = GetParam();
  const fs::path root = scratch(name);
  install(library(), name, {{"simulation", Value{simulation}}}, root);
  auto seed = toml::parse_file((root / "seed.toml").string());
  ASSERT_TRUE(seed) << seed.error().description();
  EXPECT_EQ(seed.table()["project"].value_or(std::string{}), "references");
  const auto* samples = seed.table()["samples"].as_array();
  ASSERT_NE(samples, nullptr);
  ASSERT_EQ(samples->size(), kReferences.size());
  auto defaults = experiment::DefaultsTable::load((root / "defaults.toml").string());
  ASSERT_TRUE(defaults) << defaults.error().what;
  for (std::size_t i = 0; i < kReferences.size(); ++i) {
    const auto& want = kReferences[i];
    const auto* row = (*samples)[i].as_table();
    ASSERT_NE(row, nullptr);
    EXPECT_EQ((*row)["identifier"].value_or(std::string{}), want.identifier);
    EXPECT_EQ((*row)["analysis_type"].value_or(std::string{}), experiment::to_string(want.type));
    EXPECT_EQ((*row)["sample"].value_or(std::string{}), want.sample);
    EXPECT_EQ((*row)["material"].value_or(std::string{}), want.material);
    const auto* d = defaults->find(want.type, "*");
    ASSERT_NE(d, nullptr) << want.identifier;
    EXPECT_FALSE(d->template_name.empty()) << want.identifier;
    ASSERT_TRUE(d->sample) << want.identifier;
    EXPECT_EQ(d->sample->sample, want.sample);
    EXPECT_EQ(d->sample->material, want.material);
    EXPECT_EQ(d->sample->project, "references");
  }
  for (auto t : {experiment::AnalysisType::Pause, experiment::AnalysisType::Degas}) {
    const auto* d = defaults->find(t, "*");
    EXPECT_TRUE(d == nullptr || !d->sample);
  }
  auto ids = experiment::IdentifierRules::load((root / "identifiers.toml").string());
  ASSERT_TRUE(ids) << ids.error().what;
  auto queue = experiment::load_queue_file((root / "experiment.toml").string(), *ids);
  ASSERT_TRUE(queue) << queue.error().what;
  bool blank = false;
  for (const auto& r : queue->runs) {
    if (r.id.identifier != "bu") continue;
    blank = true;
    EXPECT_EQ(r.sample.sample, "blank_unknown");
    EXPECT_EQ(r.sample.material, "blank");
    EXPECT_EQ(r.sample.project, "references");
  }
  EXPECT_TRUE(blank);
  fs::remove_all(root);
}

// The default reactor is the Triga of tests/dvc/fixtures/meta/NM-293/productions/Triga_PR.json.
TEST_P(InstrumentProfile, ShipsTheTrigaProductionOfTheNm293Fixture) {
  const auto& [name, simulation] = GetParam();
  const fs::path root = scratch(name);
  install(library(), name, {{"simulation", Value{simulation}}}, root);
  auto seed = toml::parse_file((root / "seed.toml").string());
  ASSERT_TRUE(seed) << seed.error().description();
  const auto* reactors = seed.table()["reactors"].as_table();
  ASSERT_NE(reactors, nullptr);
  EXPECT_EQ(reactors->size(), 1u);
  const auto* triga = (*reactors)["Triga"].as_table();
  ASSERT_NE(triga, nullptr);
  const std::map<std::string, std::pair<double, double>> want{
      {"K4039", {0.00873, 0.00017}}, {"K3839", {0.013, 0.0}},     {"K3739", {0.0, 0.0}},
      {"Ca3937", {0.000758, 7e-06}}, {"Ca3837", {4e-05, 2e-05}},  {"Ca3637", {0.000286, 5e-07}},
      {"Cl3638", {250.0, 0.0}},      {"Ca_K", {1.96, 0.0}},       {"Cl_K", {0.227, 0.0}}};
  EXPECT_EQ(triga->size(), want.size());
  for (const auto& [key, ve] : want) {
    const auto* pair = (*triga)[key].as_array();
    ASSERT_NE(pair, nullptr) << key;
    ASSERT_EQ(pair->size(), 2u) << key;
    EXPECT_EQ((*pair)[0].value<double>(), std::optional<double>(ve.first)) << key;
    EXPECT_EQ((*pair)[1].value<double>(), std::optional<double>(ve.second)) << key;
  }
  fs::remove_all(root);
}

INSTANTIATE_TEST_SUITE_P(Shipped, InstrumentProfile,
                         ::testing::Combine(::testing::Values("argus", "helix", "ngx"), ::testing::Bool()),
                         [](const auto& param_info) {
                           std::string n = std::get<0>(param_info.param) +
                                           (std::get<1>(param_info.param) ? "_sim" : "_hardware");
                           return n;
                         });

using Positions = std::map<std::string, std::string>;

// The hops of an installed plan, as the lab resolves it against the
// installed line and spectrometer.
std::vector<Positions> installed_hops(const fs::path& root, const SiteInstall& site, const std::string& plan) {
  auto lab = experiment::lab::load_lab({site.root, site.path(site.line), site.path(site.spectrometer)});
  EXPECT_TRUE(lab.problems.empty()) << lab.problems.front();
  auto loaded = lab.plans->load(plan, {});
  EXPECT_TRUE(loaded) << root << ": " << (loaded ? "" : loaded.error().what);
  std::vector<Positions> out;
  if (loaded)
    for (const auto& hop : loaded->plan.main.hops) out.push_back(hop.positions);
  return out;
}

TEST(Profiles, ArgusShipsTheThreeStarterPlans) {
  const auto lib = library();
  const fs::path root = scratch("argus-plans");
  const auto site = install(lib, "argus", {}, root);
  EXPECT_EQ(installed_hops(root, site, "multicollect"),
            (std::vector<Positions>{{{"Ar40", "H1"}, {"Ar39", "AX"}, {"Ar38", "L1"}, {"Ar37", "L2"}, {"Ar36", "CDD"}}}));
  // The reference isotope on each Faraday in turn; never on the ion counter.
  EXPECT_EQ(installed_hops(root, site, "detector_ic"),
            (std::vector<Positions>{{{"Ar40", "H1"}}, {{"Ar40", "AX"}}, {{"Ar40", "L1"}}, {{"Ar40", "L2"}}}));
  EXPECT_EQ(installed_hops(root, site, "multicollect_hop_ar39"),
            (std::vector<Positions>{{{"Ar40", "H1"}, {"Ar38", "L1"}, {"Ar37", "L2"}, {"Ar36", "CDD"}},
                                    {{"Ar39", "CDD"}}}));
  fs::remove_all(root);
}

TEST(Profiles, NgxShipsTheThreeStarterPlans) {
  const auto lib = library();
  const fs::path root = scratch("ngx-plans");
  const auto site = install(lib, "ngx", {}, root);
  EXPECT_EQ(installed_hops(root, site, "detector_ic"),
            (std::vector<Positions>{{{"Ar40", "H4"}}, {{"Ar40", "H3"}}, {{"Ar40", "AX"}}, {{"Ar40", "L1"}},
                                    {{"Ar40", "L2"}}}));
  EXPECT_EQ(installed_hops(root, site, "multicollect_hop_ar39"),
            (std::vector<Positions>{{{"Ar40", "H4"}, {"Ar38", "AX"}, {"Ar37", "L1"}, {"Ar36", "L2"}},
                                    {{"Ar39", "L4"}}}));
  fs::remove_all(root);
}

// A new detector_ic run in the run factory starts on the intercalibration plan.
TEST(Profiles, ADetectorIcRunDefaultsToTheIntercalibrationPlan) {
  const auto lib = library();
  const fs::path root = scratch("argus-ic");
  const auto site = install(lib, "argus", {}, root);
  std::ifstream in(root / "defaults.toml");
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  EXPECT_NE(text.find("[detector_ic.\"*\"]\ntemplate = \"detector_ic\""), std::string::npos);
  auto lab = experiment::lab::load_lab({site.root, site.path(site.line), site.path(site.spectrometer)});
  EXPECT_TRUE(lab.problems.empty()) << lab.problems.front();
  fs::remove_all(root);
}

TEST(Profiles, NgxWithALoginKeepsThePasswordInAnOwnerOnlyFile) {
  const auto lib = library();
  const fs::path root = scratch("ngx-login");
  const auto site = install(lib, "ngx",
                            {{"simulation", Value{false}},
                             {"ngx_user", Value{std::string("pychron")}},
                             {"ngx_password", Value{std::string("s3cret")}}},
                            root);
  std::string text;
  {
    std::ifstream local(root / "spectrometer.local.toml");  // closed before remove_all: Windows keeps open files
    text.assign((std::istreambuf_iterator<char>(local)), std::istreambuf_iterator<char>());
  }
  EXPECT_NE(text.find("s3cret"), std::string::npos);
#ifndef _WIN32
  const auto perms = fs::status(root / "spectrometer.local.toml").permissions();
  EXPECT_EQ(perms & (fs::perms::group_all | fs::perms::others_all), fs::perms::none);
#endif
  // The password is nowhere else.
  for (const auto& e : fs::recursive_directory_iterator(root)) {
    if (!e.is_regular_file() || e.path().filename() == "spectrometer.local.toml") continue;
    std::ifstream in(e.path());
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(body.find("s3cret"), std::string::npos) << e.path();
  }
  expect_no_failures(site);
  fs::remove_all(root);
}

TEST(Profiles, DataReductionLocalAndServer) {
  const auto lib = library();
  const fs::path local = scratch("dr-local");
  auto site = install(lib, "data-reduction", {}, local);
  EXPECT_EQ(site.kind, "data_reduction");
  EXPECT_EQ(site.database, "sqlite:" + (local / "data" / "pychron.db").generic_string());
  expect_no_failures(site);
  fs::remove_all(local);

  const fs::path server = scratch("dr-server");
  site = install(lib, "data-reduction",
                 {{"data_source", Value{std::string("server")}},
                  {"db_host", Value{std::string("db.lab.org")}},
                  {"db_user", Value{std::string("reader")}},
                  {"db_password", Value{std::string("p@ss word")}}},
                 server);
  EXPECT_EQ(site.database, "postgresql://reader@db.lab.org:5432/pychron");  // no password in the site config
  auto url = database_url(site);
  ASSERT_TRUE(url);
  EXPECT_EQ(*url, "postgresql://reader:p%40ss%20word@db.lab.org:5432/pychron");
  fs::remove_all(server);
}

TEST(Profiles, InstrumentPagesAreSimulationConnectionThenDetectors) {
  const auto r = find_resources();
  auto lib = ProfileLibrary::load(r.profiles, r.examples);
  ASSERT_TRUE(lib) << lib.error().what;
  for (const char* name : {"argus", "helix", "ngx"}) {
    auto p = lib->resolve(name);
    ASSERT_TRUE(p) << p.error().what;
    ASSERT_GE(p->groups.size(), 3u) << name;
    EXPECT_EQ(p->groups[0], "Simulation") << name;
    EXPECT_EQ(p->groups[1], "Instrument connection") << name;
    EXPECT_EQ(p->groups[2], "Detectors") << name;
  }
}
