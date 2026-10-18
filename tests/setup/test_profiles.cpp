// Every shipped profile installs with its default answers (instruments in
// simulation and on hardware), the result loads with the apps' own loaders,
// its example queue checks against the lab, and doctor finds nothing to fail.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <toml++/toml.hpp>

#include "pychron/experiment/factory/defaults.hpp"
#include "pychron/experiment/model/identifiers.hpp"

#include "pychron/experiment/lab/lab.hpp"
#include "pychron/experiment/model/queue_file.hpp"
#include "pychron/experiment/plan/parameters.hpp"
#include "pychron/experiment/plan/plan_loader.hpp"
#include "pychron/setup/doctor.hpp"
#include "pychron/setup/install.hpp"
#include "pychron/setup/installer.hpp"
#include "pychron/setup/profile.hpp"
#include "pychron/systems/spectrometer/data_dir.hpp"
#include "temp_path.hpp"

using namespace pychron;
using namespace pychron::setup;
namespace fs = std::filesystem;

namespace {

const fs::path kProfiles(PYCHRON_PROFILES_DIR);
const fs::path kExamples(PYCHRON_EXAMPLE_CONFIGS_DIR);

fs::path scratch(const std::string& tag) {
  const fs::path dir = pychron::testing::unique_temp_path("pychron-setup-" + tag + "-");
  fs::remove_all(dir);
  return dir;
}

ProfileLibrary library() {
  auto lib = ProfileLibrary::load(kProfiles, kExamples);
  EXPECT_TRUE(lib) << (lib ? "" : lib.error().what);
  return std::move(*lib);
}

std::string read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
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
  auto queue = experiment::load_queue_file((site.root / "queues" / "experiment.toml").string(), lab.ids);
  ASSERT_TRUE(queue) << queue.error().what;
  const auto check = experiment::lab::check_lab_queue(lab, *queue);
  for (const auto& d : check.all())
    EXPECT_NE(d.severity, experiment::Severity::Error) << site.profile << ": " << experiment::lab::describe(d);
}

// Every '@' alias of a plan answers: a valve name, or a time.
class AnyAlias final : public experiment::plan::IAliasResolver {
 public:
  std::optional<experiment::ParamValue> resolve_alias(std::string_view key) const override {
    if (key.starts_with("valves.")) return experiment::ParamValue{std::string("V1")};
    return experiment::ParamValue{std::int64_t{20}};
  }
};

double isotope_mass(const std::string& isotope) {
  static const std::map<std::string, double> masses{{"Ar36", 35.967545105}, {"Ar37", 36.9667759},
                                                    {"Ar38", 37.9627322},   {"Ar39", 38.964313},
                                                    {"Ar40", 39.9623831237}};
  return masses.at(isotope);
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
                        "plans/detector_ic.toml", "plans/multicollect_hop_ar39.toml", "peak_center.toml", "queues/experiment.toml",
                        "queues/blocks/blank_pair.toml", "CALIBRATE.md", "scripts/extraction/sim_extract.py", "sim.toml"})
    EXPECT_TRUE(fs::exists(root / f)) << name << ": " << f;
  // Queues and blocks live under queues/ (queue-directory design, section 2).
  EXPECT_FALSE(fs::exists(root / "experiment.toml")) << name;
  EXPECT_FALSE(fs::exists(root / "blocks")) << name;
  // The example line comes with the example lab's numbers, as they are.
  EXPECT_EQ(read_file(root / "sim.toml"), read_file(kExamples / "sim.toml")) << name;
  // And with what its laser (co2) needs: tray maps, the example tray's place
  // on its stage, its camera, and patterns, a square among them.
  for (const char* f : {"cameras.toml", "tray_maps/example-9.txt", "tray_maps/221-hole.txt",
                        "stage_calibrations/co2.example-9.toml", "patterns/square.toml", "patterns/hexagon.toml",
                        "patterns/follow.toml"}) {
    ASSERT_TRUE(fs::exists(root / f)) << name << ": " << f;
    EXPECT_EQ(read_file(root / f), read_file(kExamples / f)) << name << ": " << f;
  }
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
  auto queue = experiment::load_queue_file((root / "queues" / "experiment.toml").string(), *ids);
  ASSERT_TRUE(queue) << queue.error().what;
  // The queue is three air shots between two blanks, and every part of it is something the install has.
  std::map<std::string, std::pair<std::string, std::string>> seeded;  // identifier -> sample, material
  for (const auto& row : *samples) {
    const auto* t = row.as_table();
    ASSERT_NE(t, nullptr);
    seeded[(*t)["identifier"].value_or(std::string{})] = {(*t)["sample"].value_or(std::string{}),
                                                          (*t)["material"].value_or(std::string{})};
  }
  const auto string_list = [&](const char* key) {
    std::vector<std::string> out;
    if (const auto* a = seed.table()[key].as_array())
      for (const auto& v : *a) out.push_back(v.value_or(std::string{}));
    return out;
  };
  const auto in = [](const std::vector<std::string>& list, const std::string& wanted) {
    return std::find(list.begin(), list.end(), wanted) != list.end();
  };
  const auto spectrometers = string_list("mass_spectrometers");
  const auto devices = string_list("extract_devices");
  EXPECT_TRUE(in(spectrometers, queue->mass_spectrometer)) << name << ": " << queue->mass_spectrometer;
  EXPECT_TRUE(queue->extract_device.empty() || in(devices, queue->extract_device)) << name;
  std::vector<std::string> identifiers;
  for (const auto& r : queue->runs) {
    const std::string& id = r.id.identifier;
    identifiers.push_back(id);
    const auto row = seeded.find(id);
    ASSERT_NE(row, seeded.end()) << name << ": " << id << " is not a seed identifier";
    EXPECT_EQ(r.sample.sample, row->second.first) << id;
    EXPECT_EQ(r.sample.material, row->second.second) << id;
    EXPECT_EQ(r.sample.project, "references") << id;
    EXPECT_TRUE(r.extraction.device.empty() || in(devices, r.extraction.device)) << id;
    EXPECT_TRUE(fs::exists(root / "scripts" / "extraction" / (r.extraction.script + ".py"))) << id << ": " << r.extraction.script;
    EXPECT_TRUE(fs::exists(root / "plans" / (r.measurement.plan + ".toml"))) << id << ": " << r.measurement.plan;
    ASSERT_TRUE(r.post_measurement) << id;
    EXPECT_TRUE(fs::exists(root / "scripts" / "post_measurement" / (*r.post_measurement + ".py"))) << id << ": " << *r.post_measurement;
  }
  EXPECT_EQ(identifiers, (std::vector<std::string>{"ba", "a", "a", "a", "ba"})) << name;
  fs::remove_all(root);
}

// A hop puts one isotope on its reference detector and the rest of its isotopes on theirs by one
// magnet position; so the install's table must give those pairs the same position (to within
// half the peak-center scan window, the most the scan would move it).
TEST_P(InstrumentProfile, EveryInstalledPlanPutsItsIsotopesOnItsDetectors) {
  const auto& [name, simulation] = GetParam();
  const fs::path root = scratch(name);
  install(library(), name, {{"simulation", Value{simulation}}}, root);

  auto spec = toml::parse_file((root / "spectrometer.toml").string());
  ASSERT_TRUE(spec) << spec.error().description();
  const std::string table_name = spec.table()["magnet"]["field_table"].value_or(std::string{});
  ASSERT_FALSE(table_name.empty());
  auto file = spectrometer::cfg::resolve_current_table(root, table_name);
  ASSERT_TRUE(file) << file.error().what;
  auto table = spectrometer::parse_field_table(read_file(*file));
  ASSERT_TRUE(table) << table.error().what;

  auto peak = toml::parse_file((root / "peak_center.toml").string());
  ASSERT_TRUE(peak) << peak.error().description();
  ASSERT_GT(peak.table()["default"]["window"].value_or(0.0), 0.0);
  // The simulated peak is flat within 0.02 of its centre and gone by 0.03, and every run ends with a
  // peak center that moves the table by a few thousandths. A pair 0.01 apart would still be in the
  // flat top after such a move; one 0.02 apart has no room left, and a pair at 0.03 reads nothing.
  constexpr double tolerance = 0.01;

  const AnyAlias aliases;
  const experiment::plan::PlanResolvers resolvers{&aliases, nullptr};
  std::size_t plans = 0;
  for (const auto& entry : fs::directory_iterator(root / "plans")) {
    if (entry.path().extension() != ".toml") continue;
    ++plans;
    const std::string label = name + ": " + entry.path().filename().string();
    auto tmpl = experiment::plan::parse_plan_template(read_file(entry.path()), label);
    ASSERT_TRUE(tmpl) << label << ": " << tmpl.error().what;
    auto loaded = experiment::plan::load_plan(*tmpl, {}, resolvers);
    ASSERT_TRUE(loaded) << label << ": " << loaded.error().what;
    const auto& plan = loaded->plan;
    ASSERT_FALSE(plan.gases.empty()) << label;
    const auto* main = experiment::plan::main_segment(plan.gases.front());
    ASSERT_TRUE(main) << label;
    ASSERT_FALSE(main->positions.empty()) << label;
    int h = 0;
    std::size_t compared = 0;
    for (const auto& hop : main->positions) {
      const std::string where = label + " hop " + std::to_string(h++);
      const auto at_target = experiment::plan::position_target(plan, hop);
      ASSERT_TRUE(at_target) << where;
      const auto* target = std::get_if<experiment::plan::AtLabel>(&*at_target);
      ASSERT_TRUE(target) << where;
      ASSERT_FALSE(target->label.empty()) << where;
      auto reference = table->value_for(isotope_mass(target->label), target->detector);
      ASSERT_TRUE(reference) << where << ": " << reference.error().what;
      for (const auto& active : experiment::plan::collecting(hop)) {
        auto at = table->value_for(isotope_mass(active.label), active.detector);
        ASSERT_TRUE(at) << where << ": " << at.error().what;
        ++compared;
        EXPECT_NEAR(*at, *reference, tolerance)
            << where << ": " << active.label << " on " << active.detector << " is at " << *at << ", but "
            << target->label << " on " << target->detector << ", where the magnet is, is at " << *reference;
      }
    }
    // A plan that is only one detector per hop (detector_ic) has nothing to compare; the others do.
    if (plan.info.name != "detector_ic") {
      EXPECT_GT(compared, 1u) << label;
    }
  }
  EXPECT_EQ(plans, 3u) << name;
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

TEST_P(InstrumentProfile, HasADatabaseOnThisComputerByDefault) {
  const auto& [name, simulation] = GetParam();
  const auto lib = library();
  const fs::path root = scratch(name);
  auto site = install(lib, name, {{"simulation", Value{simulation}}}, root);
  EXPECT_EQ(site.kind, "instrument");
  EXPECT_EQ(site.database, "sqlite:" + (root / "data" / "pychron.db").generic_string());
  EXPECT_FALSE(fs::exists(root / ".pychron" / "credentials.toml"));
  // A database that is not there, or cannot be opened, is not what stops an instrument.
  DoctorOptions options;
  options.open_database = [](const std::string&) -> Result<std::string> { return fail(ErrorKind::Io, "no such file"); };
  bool said = false;
  for (const auto& c : doctor(site, options)) {
    EXPECT_NE(c.status, Check::Status::Fail) << c.name << ": " << c.detail;
    said |= c.name == "database" && c.status == Check::Status::Warn;
  }
  EXPECT_TRUE(said);
  options.open_database = [](const std::string&) -> Result<std::string> { return std::string("schema version 4"); };
  bool ok = false;
  for (const auto& c : doctor(site, options)) ok |= c.name == "database" && c.status == Check::Status::Ok;
  EXPECT_TRUE(ok);
  fs::remove_all(root);

  const fs::path server = scratch(name + "-server");
  site = install(lib, name,
                 {{"simulation", Value{simulation}},
                  {"data_source", Value{std::string("server")}},
                  {"db_host", Value{std::string("db.lab.org")}},
                  {"db_user", Value{std::string("jan")}},
                  {"db_password", Value{std::string("p@ss word")}}},
                 server);
  EXPECT_EQ(site.database, "postgresql://jan@db.lab.org:5432/pychron");
  auto url = database_url(site);
  ASSERT_TRUE(url) << url.error().what;
  EXPECT_EQ(*url, "postgresql://jan:p%40ss%20word@db.lab.org:5432/pychron");
  fs::remove_all(server);
}

INSTANTIATE_TEST_SUITE_P(Shipped, InstrumentProfile,
                         ::testing::Combine(::testing::Values("argus", "helix", "ngx"), ::testing::Bool()),
                         [](const auto& param_info) {
                           std::string n = std::get<0>(param_info.param) +
                                           (std::get<1>(param_info.param) ? "_sim" : "_hardware");
                           return n;
                         });

using Positions = std::map<std::string, std::string>;

// The peak center step a plan runs after the run (v1 [peak_center] after); nullptr: none.
const experiment::plan::PeakCenterStep* after_run_peak_center(const experiment::plan::MeasurementPlan& plan) {
  for (const auto& gas : plan.gases)
    for (const auto& seg : gas.segments)
      if (seg.peak_center && seg.when == experiment::plan::When::AfterRun) return &*seg.peak_center;
  return nullptr;
}

// The kind of time zero of a plan's main segment (v1 [main] time_zero).
std::optional<experiment::plan::TimeZeroKind> main_time_zero(const experiment::plan::MeasurementPlan& plan) {
  if (plan.gases.empty()) return std::nullopt;
  const auto* main = experiment::plan::main_segment(plan.gases.front());
  if (main == nullptr || !main->time_zero) return std::nullopt;
  return main->time_zero->kind;
}

// The hops of an installed plan, as the lab resolves it against the
// installed line and spectrometer.
std::vector<Positions> installed_hops(const fs::path& root, const SiteInstall& site, const std::string& plan) {
  auto lab = experiment::lab::load_lab({site.root, site.path(site.line), site.path(site.spectrometer)});
  EXPECT_TRUE(lab.problems.empty()) << lab.problems.front();
  auto loaded = lab.plans->load(plan, {});
  EXPECT_TRUE(loaded) << root << ": " << (loaded ? "" : loaded.error().what);
  std::vector<Positions> out;
  if (!loaded || loaded->plan.gases.empty()) return out;
  if (const auto* main = experiment::plan::main_segment(loaded->plan.gases.front())) {
    for (const auto& pos : main->positions) {
      Positions hop;
      for (const auto& e : experiment::plan::collecting(pos)) hop[e.label] = e.detector;
      out.push_back(std::move(hop));
    }
  }
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

// Every shipped plan says where its time zero is and lets a run change it:
// the measurement panel shows what a plan exposes.
// Every shipped plan leaves the field table alone after a run with next to
// no signal (legacy's pre-check, 3), and lets a run change the minimum.
TEST(Profiles, EveryShippedPlanHasAPeakCenterMinimumSignalAndShowsIt) {
  namespace plan = experiment::plan;
  const auto lib = library();
  const fs::path root = scratch("argus-peak-center-minimum");
  const auto site = install(lib, "argus", {}, root);
  auto lab = experiment::lab::load_lab({site.root, site.path(site.line), site.path(site.spectrometer)});
  ASSERT_TRUE(lab.problems.empty()) << lab.problems.front();
  for (const char* name : {"multicollect", "detector_ic", "multicollect_hop_ar39"}) {
    const auto* tmpl = lab.plans->find(name);
    ASSERT_NE(tmpl, nullptr) << name;
    const auto params = plan::plan_parameters(*tmpl);
    ASSERT_TRUE(params) << name << ": " << params.error().what;
    const auto shown =
        std::find_if(params->begin(), params->end(), [](const auto& p) { return p.path == "peak_center.min_signal"; });
    ASSERT_NE(shown, params->end()) << name;
    EXPECT_EQ(shown->label, "Peak center minimum signal") << name;
    EXPECT_EQ(shown->value, experiment::ParamValue(3.0)) << name;

    auto as_shipped = lab.plans->load(name, {});
    ASSERT_TRUE(as_shipped) << name << ": " << as_shipped.error().what;
    const auto* after = after_run_peak_center(as_shipped->plan);
    ASSERT_TRUE(after) << name;
    EXPECT_DOUBLE_EQ(after->min_signal, 3.0) << name;
    auto changed = lab.plans->load(name, {{"peak_center.min_signal", 12.5}});
    ASSERT_TRUE(changed) << name << ": " << changed.error().what;
    ASSERT_TRUE(after_run_peak_center(changed->plan)) << name;
    EXPECT_DOUBLE_EQ(after_run_peak_center(changed->plan)->min_signal, 12.5) << name;
    EXPECT_FALSE(lab.plans->load(name, {{"peak_center.min_signal", -1.0}})) << name;
  }
  fs::remove_all(root);
}

TEST(Profiles, EveryShippedPlanShowsItsTimeZero) {
  namespace plan = experiment::plan;
  const auto lib = library();
  const fs::path root = scratch("argus-time-zero");
  const auto site = install(lib, "argus", {}, root);
  auto lab = experiment::lab::load_lab({site.root, site.path(site.line), site.path(site.spectrometer)});
  ASSERT_TRUE(lab.problems.empty()) << lab.problems.front();
  for (const char* name : {"multicollect", "detector_ic", "multicollect_hop_ar39"}) {
    const auto* tmpl = lab.plans->find(name);
    ASSERT_NE(tmpl, nullptr) << name;
    const auto params = plan::plan_parameters(*tmpl);
    ASSERT_TRUE(params) << name << ": " << params.error().what;
    const auto shown = std::find_if(params->begin(), params->end(), [](const auto& p) { return p.path == "main.time_zero"; });
    ASSERT_NE(shown, params->end()) << name;
    EXPECT_EQ(shown->label, "Time zero") << name;
    EXPECT_EQ(shown->value, experiment::ParamValue(std::string("on_inlet_close"))) << name;

    auto as_shipped = lab.plans->load(name, {});
    ASSERT_TRUE(as_shipped) << name << ": " << as_shipped.error().what;
    EXPECT_EQ(main_time_zero(as_shipped->plan), plan::TimeZeroKind::OnInletClose) << name;
    auto changed = lab.plans->load(name, {{"main.time_zero", std::string("on_first_count")}});
    ASSERT_TRUE(changed) << name << ": " << changed.error().what;
    EXPECT_EQ(main_time_zero(changed->plan), plan::TimeZeroKind::OnFirstCount) << name;
    EXPECT_FALSE(lab.plans->load(name, {{"main.time_zero", std::string("soon")}})) << name;
  }
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

// The example lab's numbers belong to the example line: a line the lab brings is not told them.
TEST(Profiles, AnImportedLineComesWithoutTheExampleLabsSimToml) {
  const auto lib = library();
  const fs::path root = scratch("imported-line");
  const fs::path mine = fs::temp_directory_path() / (root.filename().string() + "-mine");
  fs::create_directories(mine);
  fs::copy_file(kExamples / "extraction_line.toml", mine / "line.toml", fs::copy_options::overwrite_existing);
  fs::copy_file(kExamples / "canvas.toml", mine / "drawing.toml", fs::copy_options::overwrite_existing);
  install(lib, "argus",
          {{"line_source", Value{std::string("import")}},
           {"line_file", Value{(mine / "line.toml").generic_string()}},
           {"canvas_file", Value{(mine / "drawing.toml").generic_string()}}},
          root);
  EXPECT_TRUE(fs::exists(root / "extraction_line.toml"));
  EXPECT_FALSE(fs::exists(root / "sim.toml"));
  // Nor with the camera and stage calibration of the example line's laser;
  // tray maps and patterns are no device's, and come with any line.
  EXPECT_FALSE(fs::exists(root / "cameras.toml"));
  EXPECT_FALSE(fs::exists(root / "stage_calibrations"));
  EXPECT_TRUE(fs::exists(root / "tray_maps" / "221-hole.txt"));
  EXPECT_TRUE(fs::exists(root / "patterns" / "square.toml"));
  fs::remove_all(root);
  fs::remove_all(mine);
}

// doctor tells an install made before the seed, the identifiers and the
// database that its profile has new files: only a higher version says so.
TEST(Profiles, AnInstallFromBeforeTheSeedIsToldItsProfileIsNewer) {
  const auto lib = library();
  const fs::path root = scratch("old-argus");
  auto site = install(lib, "argus", {}, root);
  // As an install of the profiles before this work recorded them.
  const fs::path record = record_path(root);
  std::string text;
  {
    std::ifstream in(record);
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  const std::string now = "instrument-common = " + std::to_string(lib.find("instrument-common")->version);
  const auto at = text.find(now);
  ASSERT_NE(at, std::string::npos) << text;
  text.replace(at, now.size(), "instrument-common = 1");
  { std::ofstream(record) << text; }
  DoctorOptions options;
  options.library = &lib;
  bool told = false;
  for (const auto& c : doctor(site, options)) told |= c.name == "profile instrument-common" && c.status == Check::Status::Warn;
  EXPECT_TRUE(told);
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

TEST(Profiles, StoreCommonAsksForTheFileAndTheDataFolder) {
  const auto lib = library();
  const Profile* store = lib.find("store-common");
  ASSERT_NE(store, nullptr);
  EXPECT_EQ(store->version, 2);
  auto question = [&](const std::string& id) -> const Question* {
    const auto it = std::ranges::find(store->questions, id, &Question::id);
    return it == store->questions.end() ? nullptr : &*it;
  };
  const Question* file = question("db_file");
  ASSERT_NE(file, nullptr);
  EXPECT_EQ(file->type, QuestionType::Path);
  ASSERT_TRUE(file->default_value.has_value());
  EXPECT_EQ(*file->default_value, Value{std::string("data/pychron.db")});
  EXPECT_EQ(file->group, "Data");
  EXPECT_EQ(file->when, "data_source == \"local\"");
  EXPECT_EQ(file->prompt, "Database file");
  EXPECT_EQ(file->help, "Relative to the install folder, or a full path");

  const Question* folder = question("data_folder");
  ASSERT_NE(folder, nullptr);
  EXPECT_EQ(folder->type, QuestionType::Path);
  ASSERT_TRUE(folder->default_value.has_value());
  EXPECT_EQ(*folder->default_value, Value{std::string("data")});
  EXPECT_EQ(folder->group, "Data");
  EXPECT_TRUE(folder->when.empty());
  EXPECT_EQ(folder->prompt, "Data folder");
  EXPECT_EQ(folder->help, "Where run records are kept");
}

TEST(Profiles, ADataFolderAnswerIsTheSiteEntrysData) {
  const auto lib = library();
  auto resolved = lib.resolve("data-reduction");
  ASSERT_TRUE(resolved) << resolved.error().what;
  const fs::path root = scratch("dr-data-folder");
  const std::string records = (fs::temp_directory_path() / "x" / "records-disk").generic_string();
  const Answers builtins{{"install_name", Value{std::string("dr")}}, {"root", Value{root.generic_string()}}};

  auto answers = complete_answers(*resolved, {{"data_folder", Value{records}}}, builtins);
  ASSERT_TRUE(answers) << answers.error().what;
  auto plan = plan_install(lib, *resolved, *answers, root);
  ASSERT_TRUE(plan) << plan.error().what;
  EXPECT_EQ(site_install(*plan, "dr").data, records);

  plan->answers.erase("data_folder");
  EXPECT_EQ(site_install(*plan, "dr").data, "data");
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
    EXPECT_EQ(p->groups.back(), "Data") << name;
  }
}
