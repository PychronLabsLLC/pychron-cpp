// A fresh instrument install, end to end: the profile is installed into a
// scratch folder with its default answers, its database made and seeded, its
// own line and spectrometer brought up simulated on a VirtualClock, and the
// queue it ships with run through the store persister. Every run is then read
// back the way the data window reads it (processing::StoreSource) and held
// against what the records path makes of the very record that was saved.
//
// The test's thread takes part in the clock: simulated time moves only while
// it waits there (session.wait()). The store's worker threads do not, so a
// save costs no simulated time.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "pychron/core/virtual_clock.hpp"
#include "pychron/entry/seed.hpp"
#include "pychron/experiment/lab/session.hpp"
#include "pychron/experiment/model/queue_file.hpp"
#include "pychron/experiment/record/names.hpp"
#include "pychron/experiment/store/store_persister.hpp"
#include "pychron/persistence/blob.hpp"
#include "pychron/persistence/store.hpp"
#include "pychron/processing/record_source.hpp"
#include "pychron/processing/store_source.hpp"
#include "pychron/scripting/script_host.hpp"
#include "pychron/setup/doctor.hpp"
#include "pychron/setup/install.hpp"
#include "pychron/setup/installer.hpp"
#include "pychron/setup/profile.hpp"
#include "pychron/setup/site.hpp"
#include "pychron/sim/gas.hpp"
#include "pychron/sim/spectrometer/beam_model.hpp"
#include "pychron/systems/extraction_line.hpp"
#include "pychron/systems/spectrometer/bringup.hpp"
#include "store_fixture.hpp"
#include "virtual_time.hpp"

namespace pychron::experiment {
namespace {

namespace fs = std::filesystem;
namespace ps = pychron::persistence;
using namespace std::chrono_literals;

// An environment variable set for the life of this object, then put back.
class ScopedEnv {
 public:
  ScopedEnv(std::string name, const std::string& value) : name_(std::move(name)) {
    if (const char* old = std::getenv(name_.c_str())) old_ = old;
    ::setenv(name_.c_str(), value.c_str(), 1);
  }
  ~ScopedEnv() {
    if (old_) ::setenv(name_.c_str(), old_->c_str(), 1);
    else ::unsetenv(name_.c_str());
  }
  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;
  ScopedEnv(ScopedEnv&&) = delete;
  ScopedEnv& operator=(ScopedEnv&&) = delete;

 private:
  std::string name_;
  std::optional<std::string> old_;
};

// One call of save_analysis: the record it was given, whether it was taken,
// and what the call cost in real time.
struct Saved {
  record::AnalysisRecord record;
  bool ok = false;
  std::string error;
  std::chrono::duration<double, std::milli> took{};
};

// Hands every call on to `next` unchanged, and keeps a copy of every record
// given to save_analysis. Called on the queue's thread; read once the queue
// has ended.
class KeepingPersister final : public persist::IAnalysisPersister {
 public:
  explicit KeepingPersister(persist::IAnalysisPersister& next) : next_(next) {}

  Result<int> next_aliquot(const std::string& identifier) override { return next_.next_aliquot(identifier); }
  Result<void> begin_run(const RunIdentity& id, const QueueSpec& queue) override { return next_.begin_run(id, queue); }
  Result<void> save_extraction(const record::AnalysisRecord& r) override { return next_.save_extraction(r); }
  Result<void> save_analysis(const record::AnalysisRecord& r) override {
    // Real time on purpose: what a save costs the operator. Nothing is
    // asserted of it.
    const auto began = std::chrono::steady_clock::now();
    auto result = next_.save_analysis(r);
    Saved saved{r, result.has_value(), result ? std::string() : result.error().what,
                std::chrono::steady_clock::now() - began};
    std::lock_guard lock(mutex_);
    saved_.push_back(std::move(saved));
    return result;
  }
  Result<void> save_artifact(const std::string& uuid, const std::string& name,
                             const std::vector<std::uint8_t>& bytes) override {
    return next_.save_artifact(uuid, name, bytes);
  }
  Result<void> flush() override { return next_.flush(); }
  Result<std::vector<std::string>> check_queue(const QueueSpec& queue) override { return next_.check_queue(queue); }

  std::vector<Saved> saved() {
    std::lock_guard lock(mutex_);
    return saved_;
  }

 private:
  persist::IAnalysisPersister& next_;
  std::mutex mutex_;
  std::vector<Saved> saved_;
};

std::string read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// What one run of the queue left behind.
struct Ran {
  std::optional<executor::QueueResult> result;
  std::vector<Saved> saved;
  std::size_t pending = 0;
};

class InstallE2E : public pychron::testing::VirtualTimeTest, public ::testing::WithParamInterface<std::string> {
 protected:
  void SetUp() override {
    if (!scripting::scripting_enabled()) GTEST_SKIP() << "the shipped queue's scripts need PYCHRON_SCRIPTING";
    dir_ = fs::temp_directory_path() /
           ("pychron-e2e-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(std::random_device{}()));
    fs::create_directories(dir_);
    site_config_.emplace("PYCHRON_SITE_CONFIG", (dir_ / "site.toml").string());
    install();
    if (HasFatalFailure()) return;
    make_database();
    if (HasFatalFailure()) return;
    bring_up();
  }

  // The profile with its default answers, as `elctl init argus --yes` plans
  // and writes it.
  void install() {
    const fs::path root = dir_ / "lab";
    auto library = setup::ProfileLibrary::load(fs::path(PYCHRON_PROFILES_DIR), fs::path(PYCHRON_EXAMPLE_CONFIGS_DIR));
    ASSERT_TRUE(library) << library.error().what;
    auto resolved = library->resolve("argus");
    ASSERT_TRUE(resolved) << resolved.error().what;
    const std::string name = setup::default_install_name(resolved->top);
    auto answers = setup::complete_answers(*resolved, {}, setup::builtin_answers(name, root));
    ASSERT_TRUE(answers) << answers.error().what;
    auto plan = setup::plan_install(*library, *resolved, *answers, root);
    ASSERT_TRUE(plan) << plan.error().what;
    auto report = setup::apply_install(*plan);
    ASSERT_TRUE(report) << report.error().what;
    site_ = setup::site_install(*plan, name);
    ASSERT_EQ(site_.kind, "instrument");
    ASSERT_TRUE(site_.simulation);  // the default answer
    ASSERT_FALSE(site_.database.empty());
    const fs::path site_path = setup::default_site_path();
    ASSERT_EQ(site_path, dir_ / "site.toml");
    auto registered = setup::register_install(site_, site_path);
    ASSERT_TRUE(registered) << registered.error().what;
  }

  // The install's database, made and seeded from the install's seed.toml. On
  // PostgreSQL the install's url gives way to the test schema's.
  void make_database() {
    if (GetParam() == "pg") {
      pg_.emplace("pg", false);
      url_ = pg_->url();
    } else {
      auto url = setup::database_url(site_);
      ASSERT_TRUE(url) << url.error().what;
      url_ = *url;
      ASSERT_TRUE(url_.starts_with("sqlite:")) << url_;
      // SQLite makes the file, not the folder it is in.
      fs::create_directories(fs::path(url_.substr(std::string("sqlite:").size())).parent_path());
    }
    auto store = ps::open_store(ps::StoreConfig{url_, true});
    ASSERT_TRUE(store) << store.error().what;
    const fs::path seed_file = site_.root / "seed.toml";
    auto seed = entry::parse_seed(read_file(seed_file), seed_file.string());
    ASSERT_TRUE(seed) << seed.error().what;
    seed_ = *seed;
    auto client = (*store)->register_client({"localhost", "reduction", std::nullopt, "test"});
    ASSERT_TRUE(client) << client.error().what;
    auto user = (*store)->ensure_user(*client, "pychron");
    ASSERT_TRUE(user) << user.error().what;
    auto report = entry::apply_seed(**store, seed_, ps::Actor{*user, *client});
    ASSERT_TRUE(report) << report.error().what;
    EXPECT_GT(report->identifiers, 0);
    EXPECT_EQ(report->mass_spectrometers, 1);
  }

  // The install's own line and spectrometer, simulated, on the clock; the
  // beam measures the line's gas, as in the applications.
  void bring_up() {
    systems::ExtractionLine::Options options;
    options.clock = &clock_;
    options.force_sim = site_.simulation;
    std::optional<fs::path> canvas;
    if (!site_.canvas.empty()) canvas = site_.path(site_.canvas);
    auto line = systems::ExtractionLine::load(site_.path(site_.line), canvas, options);
    ASSERT_TRUE(line) << line.error().what;
    line_ = std::move(*line);
    ASSERT_TRUE(line_->start());

    spectrometer::BeamFeed fed = spectrometer::BeamFeed::FixedGas;
    auto spec = spectrometer::load_spectrometer_for_app(
        site_.path(site_.spectrometer), spectrometer::SpectrometerContext{clock_, line_->scheduler(), line_->bus()},
        spectrometer::SpectrometerBringup{
            .sim_beam_from_table = true, .require_sim = true, .line_sim = line_->sim(), .fed = &fed});
    ASSERT_TRUE(spec) << spec.error().what;
    spec_ = std::move(*spec);
    ASSERT_EQ(fed, spectrometer::BeamFeed::LineGas);

    lab_ = lab::load_lab({site_.root, site_.path(site_.line), site_.path(site_.spectrometer)});
    ASSERT_TRUE(lab_.problems.empty()) << lab_.problems.front();
    auto queue = load_queue_file((site_.root / "experiment.toml").string(), lab_.ids);
    ASSERT_TRUE(queue) << queue.error().what;
    queue_ = *queue;
  }

  void TearDown() override {
    if (line_) line_->stop();
    spec_.reset();
    sim::BeamModelRegistry::global().clear();
    line_.reset();
    std::error_code ignored;
    fs::remove_all(dir_, ignored);
  }

  // Runs the shipped queue in a session of its own, saving through a store
  // opened for it: what one `elctl exp run` of the install does.
  Ran run_queue() { return run_queue(queue_); }

  Ran run_queue(const QueueSpec& queue) {
    Ran out;
    const fs::path data = site_.path(site_.data);
    // Before the session and everything else that can call its persister.
    auto acquisition = store::open_acquisition(url_, data, "test");
    if (!acquisition) {
      ADD_FAILURE() << acquisition.error().what;
      return out;
    }
    KeepingPersister keeping((*acquisition)->persister());
    // As the applications do: the install simulates, and its records say so.
    lab::LabSession session(lab_, lab::SessionHardware{*line_, spec_.get(), nullptr, {}, nullptr, site_.simulation},
                            lab::SessionOptions{data, {}, {}, &keeping});
    for (const auto& p : session.problems()) ADD_FAILURE() << p;
    if (auto started = session.start(queue); !started) {
      ADD_FAILURE() << started.error().what;
      return out;
    }
    out.result = session.wait();
    out.pending = session.pending_saves();
    out.saved = keeping.saved();
    return out;
  }

  static void expect_five_good_runs(const Ran& ran) {
    ASSERT_TRUE(ran.result.has_value());
    EXPECT_EQ(ran.result->end, executor::QueueEnd::Completed) << ran.result->reason;
    ASSERT_EQ(ran.result->runs.size(), 5u);
    for (const auto& r : ran.result->runs)
      EXPECT_EQ(r.state, run::RunState::Success) << r.identifier << " " << r.error.value_or("");
    EXPECT_EQ(ran.pending, 0u);
    ASSERT_EQ(ran.saved.size(), 5u);
    for (const auto& s : ran.saved) {
      EXPECT_TRUE(s.ok) << s.record.identity.identifier << ": " << s.error;
      EXPECT_TRUE(s.record.instrument.simulated) << s.record.identity.identifier;
    }
  }

  // Run ids as the data window lists them: newest first.
  std::vector<processing::AnalysisSummary> browse() const {
    auto source = processing::StoreSource::open(ps::StoreConfig{url_, false});
    if (!source) {
      ADD_FAILURE() << source.error().what;
      return {};
    }
    auto page = (*source)->browse({});
    if (!page) {
      ADD_FAILURE() << page.error().what;
      return {};
    }
    return page->rows;
  }

  static std::vector<std::string> runids(const std::vector<processing::AnalysisSummary>& rows) {
    std::vector<std::string> ids;
    ids.reserve(rows.size());
    for (const auto& r : rows) ids.push_back(r.runid);
    return ids;
  }

  std::chrono::steady_clock::time_point began_ = std::chrono::steady_clock::now();
  fs::path dir_;
  std::optional<ScopedEnv> site_config_;
  VirtualClock clock_;  // before everything that is given a reference to it
  Clock::Participant test_{clock_, "test"};
  std::optional<ps::testing::TestDatabase> pg_;  // the PostgreSQL case's schema
  std::string url_;
  setup::SiteInstall site_;
  entry::Seed seed_;
  std::unique_ptr<systems::ExtractionLine> line_;
  std::unique_ptr<spectrometer::Spectrometer> spec_;
  lab::Lab lab_;
  QueueSpec queue_;
};

// The loaded analysis against what the records path makes of the record that
// was saved: identity, and each isotope's key, name, detector, intercept,
// count, baseline and the two fits.
//
// Not compared, because the store does not take them from the record: sample,
// project and material (the catalog's), time zero, events and notes, peak
// centers, script texts.
//
// Not compared, although both paths carry them: the extraction fields (value,
// units, duration, cleanup, positions, pattern, beam diameter, cryo
// temperature: the records path prefers the actuals, the store has the
// request), the gains and deflections, the source parameters, the analyst and
// the extract device. A loss or a mix-up of one of those passes this helper.
void expect_same_analysis(const processing::Analysis& got, const processing::Analysis& want) {
  EXPECT_EQ(got.uuid, want.uuid);
  EXPECT_EQ(got.identifier, want.identifier);
  EXPECT_EQ(got.aliquot, want.aliquot);
  EXPECT_EQ(got.increment, want.increment);
  EXPECT_EQ(got.runid, want.runid);
  EXPECT_EQ(got.analysis_type, want.analysis_type);
  EXPECT_EQ(got.mass_spectrometer, want.mass_spectrometer);
  EXPECT_NEAR(got.timestamp, want.timestamp, 1e-3);

  ASSERT_FALSE(want.isotopes.empty());
  ASSERT_EQ(got.isotopes.size(), want.isotopes.size());
  for (std::size_t i = 0; i < want.isotopes.size(); ++i) {
    const auto& g = got.isotopes[i];
    const auto& w = want.isotopes[i];
    SCOPED_TRACE(w.key);
    EXPECT_EQ(g.key, w.key);
    EXPECT_EQ(g.isotope, w.isotope);
    EXPECT_EQ(g.detector, w.detector);
    EXPECT_FALSE(w.detector.empty());
    EXPECT_EQ(g.intercept.value, w.intercept.value);
    EXPECT_EQ(g.intercept.error, w.intercept.error);
    EXPECT_EQ(g.n, w.n);
    EXPECT_EQ(g.baseline.value, w.baseline.value);
    EXPECT_EQ(g.baseline.error, w.baseline.error);
    ASSERT_TRUE(w.fit.has_value());
    ASSERT_TRUE(g.fit.has_value());
    EXPECT_TRUE(record::same_fit(*g.fit, *w.fit));
    ASSERT_TRUE(w.baseline_fit.has_value());
    ASSERT_TRUE(g.baseline_fit.has_value());
    EXPECT_TRUE(record::same_fit(*g.baseline_fit, *w.baseline_fit));
  }
}

// Every raw series of the record, point for point, as the source decodes it.
void expect_same_raw(const processing::RawData& got, const processing::RawData& want) {
  ASSERT_FALSE(want.series.empty());
  ASSERT_EQ(got.series.size(), want.series.size());
  for (const auto& w : want.series) {
    SCOPED_TRACE(std::string(processing::to_string(w.kind)) + " " + w.key);
    const auto* g = got.find(w.kind, w.key);
    ASSERT_NE(g, nullptr);
    EXPECT_EQ(g->detector, w.detector);
    EXPECT_FALSE(w.t.empty());
    EXPECT_EQ(g->t, w.t);
    EXPECT_EQ(g->v, w.v);
  }
}

// The raw model of the data window has no sigma, so the stored blobs are read
// directly: a series the record gives a sigma has it, point for point, in the
// store. Returns how many series had one. A queue none of whose series has a
// sigma returns 0 and has shown nothing about them: the caller says how many
// there were, and a test that depends on them must require the count.
int expect_same_sigmas(ps::IStore& store, const record::AnalysisRecord& rec) {
  int with_sigma = 0;
  const auto id = ps::Uuid::parse(rec.identity.uuid);
  EXPECT_TRUE(id.has_value());
  if (!id) return 0;
  auto view = store.load_analysis(*id);
  EXPECT_TRUE(view && view->has_value());
  if (!view || !*view) return 0;
  const auto payload = (*view)->payloads.find(ps::Kind::Signals);
  EXPECT_NE(payload, (*view)->payloads.end());
  if (payload == (*view)->payloads.end()) return 0;
  const auto* refs = std::get_if<ps::SignalRefs>(&payload->second);
  EXPECT_NE(refs, nullptr);
  if (refs == nullptr) return 0;
  // As an analysis names them: a detector's baseline series are one stored series.
  const auto named = record::named_series(rec.data);
  EXPECT_EQ(refs->size(), named.size());
  for (const auto& s : named) {
    SCOPED_TRACE(s.kind + " " + s.key);
    const ps::SignalRefRow* ref = nullptr;
    for (const auto& r : *refs)
      if (r.series_kind == s.kind && r.series_key == s.key) ref = &r;
    EXPECT_NE(ref, nullptr);
    if (ref == nullptr) continue;
    EXPECT_EQ(ref->detector, s.detector);
    auto blob = store.load_blob(ref->blob_sha);
    EXPECT_TRUE(blob && blob->has_value());
    if (!blob || !*blob) continue;
    EXPECT_EQ((*blob)->codec, s.trace.sigma.empty() ? ps::kCodecTv : ps::kCodecTvs);
    if (s.trace.sigma.empty() || (*blob)->codec != ps::kCodecTvs) continue;
    ++with_sigma;
    auto points = ps::decode_tvs((*blob)->bytes);
    EXPECT_TRUE(points.has_value());
    if (!points) continue;
    std::vector<float> sigma;
    sigma.reserve(points->size());
    for (const auto& p : *points) sigma.push_back(p.s);
    EXPECT_EQ(sigma, s.trace.sigma);
  }
  return with_sigma;
}

TEST_P(InstallE2E, AFreshInstallsQueueLandsInItsDatabase) {
  const TimePoint queue_began = clock_.now();
  const Ran ran = run_queue();
  ASSERT_NO_FATAL_FAILURE(expect_five_good_runs(ran));
  // The queue's own time passed on the clock (19 min 15 s of it, measured).
  EXPECT_GT(clock_.now() - queue_began, 15min);

  // What a save costs in real time; a measurement, not an assertion.
  double total_ms = 0;
  for (const auto& s : ran.saved) {
    std::printf("save %s-%02d (%s): %.1f ms\n", s.record.identity.identifier.c_str(), s.record.identity.aliquot,
                GetParam().c_str(), s.took.count());
    total_ms += s.took.count();
  }
  std::printf("save mean (%s): %.1f ms over %zu runs\n", GetParam().c_str(),
              total_ms / static_cast<double>(ran.saved.size()), ran.saved.size());

  // The data window's reader, opened on this thread.
  auto opened = processing::StoreSource::open(ps::StoreConfig{url_, false});
  ASSERT_TRUE(opened) << opened.error().what;
  processing::StoreSource& source = **opened;
  auto page = source.browse({});
  ASSERT_TRUE(page) << page.error().what;
  EXPECT_EQ(runids(page->rows), (std::vector<std::string>{"ba-02", "a-03", "a-02", "a-01", "ba-01"}));
  std::map<std::string, std::string> seed_sample;  // identifier -> the seed's sample
  for (const auto& s : seed_.samples) seed_sample[s.identifier] = s.sample;
  for (const auto& row : page->rows) {
    SCOPED_TRACE(row.runid);
    EXPECT_EQ(row.project, seed_.project);
    EXPECT_EQ(row.project, "references");
    ASSERT_TRUE(seed_sample.contains(row.identifier));
    EXPECT_EQ(row.sample, seed_sample[row.identifier]);
    EXPECT_EQ(row.mass_spectrometer, "argus");
  }

  // Every analysis has all of its raw series (signals_state).
  auto store = ps::open_store(ps::StoreConfig{url_, false});
  ASSERT_TRUE(store) << store.error().what;
  auto found = (*store)->find_analyses({});
  ASSERT_TRUE(found) << found.error().what;
  ASSERT_EQ(found->size(), 5u);
  for (const auto& a : *found) EXPECT_EQ(a.signals_state, "complete") << a.runid;

  std::vector<processing::AnalysisPtr> airs, blanks;
  int series_with_sigma = 0;
  for (const auto& s : ran.saved) {
    const auto& rec = s.record;
    SCOPED_TRACE(rec.identity.identifier + "-" + std::to_string(rec.identity.aliquot));
    auto loaded = source.load(rec.identity.uuid);
    ASSERT_TRUE(loaded) << loaded.error().what;
    ASSERT_NE(*loaded, nullptr);
    expect_same_analysis(**loaded, processing::analysis_from_record(rec));
    EXPECT_EQ((*loaded)->project, seed_.project);
    EXPECT_EQ((*loaded)->sample, seed_sample[rec.identity.identifier]);
    auto raw = source.load_raw(rec.identity.uuid);
    ASSERT_TRUE(raw) << raw.error().what;
    expect_same_raw(*raw, processing::raw_from_record(rec));
    series_with_sigma += expect_same_sigmas(**store, rec);
    ((*loaded)->analysis_type == "air" ? airs : blanks).push_back(*loaded);
  }
  std::printf("series with a sigma: %d\n", series_with_sigma);
  ASSERT_EQ(airs.size(), 3u);
  ASSERT_EQ(blanks.size(), 2u);

  // An air is an air and a blank a blank. The simulated CDD (Ar36, an ion
  // counter read in cps) counts 0.9847 of what reaches it
  // (BeamModel::counter_yield, libs/sim/src/spectrometer/beam_model.cpp), so
  // the raw Ar40/Ar36 of air is 298.56 / 0.9847 = 303.2. Measured on a fresh
  // argus install: mean 305.8, 0.54 % scatter across the three airs; Ar40
  // about 94 470 fA in an air and about 11 fA in a blank.
  const double expected = sim::air_ratios()[sim::index(sim::Species::Ar40)] / 0.9847;
  for (const auto& air : airs) {
    SCOPED_TRACE(air->runid);
    const auto* ar40 = air->find_by_isotope("Ar40");
    const auto* ar36 = air->find_by_isotope("Ar36");
    ASSERT_NE(ar40, nullptr);
    ASSERT_NE(ar36, nullptr);
    EXPECT_EQ(ar36->detector, "CDD");
    ASSERT_GT(ar36->intercept.value, 0.0);
    const double ratio = ar40->intercept.value / ar36->intercept.value;
    std::printf("%s: Ar40 %.1f, Ar36 %.2f, Ar40/Ar36 %.2f\n", air->runid.c_str(), ar40->intercept.value,
                ar36->intercept.value, ratio);
    EXPECT_NEAR(ratio, expected, 0.03 * expected);
    for (const auto& blank : blanks) {
      const auto* blank40 = blank->find_by_isotope("Ar40");
      ASSERT_NE(blank40, nullptr);
      EXPECT_GT(ar40->intercept.value, 1000.0 * blank40->intercept.value) << blank->runid;
    }
  }
  for (const auto& blank : blanks)
    std::printf("%s: Ar40 %.2f\n", blank->runid.c_str(), blank->find_by_isotope("Ar40")->intercept.value);

  // 19 min 15 s of the lab's time, with the install and the database made
  // first: a bound in real time that only tells "took no real time" from a stall.
  EXPECT_LT(std::chrono::steady_clock::now() - began_, 30s);
}

// A second session on the same database goes on numbering where the first
// stopped: the aliquots come from the store, not from a persister's memory.
TEST_P(InstallE2E, RunningItAgainNumbersOn) {
  const Ran first = run_queue();
  ASSERT_NO_FATAL_FAILURE(expect_five_good_runs(first));
  EXPECT_EQ(runids(browse()), (std::vector<std::string>{"ba-02", "a-03", "a-02", "a-01", "ba-01"}));

  const Ran second = run_queue();
  ASSERT_NO_FATAL_FAILURE(expect_five_good_runs(second));
  EXPECT_EQ(runids(browse()), (std::vector<std::string>{"ba-04", "a-06", "a-05", "a-04", "ba-03", "ba-02", "a-03",
                                                         "a-02", "a-01", "ba-01"}));
  EXPECT_LT(std::chrono::steady_clock::now() - began_, 30s);
}

// Every plan the install ships is run once, and so is one the test writes
// with a baseline block and a baseline hop on one detector (no shipped plan
// has that shape): each record is taken by the store, and reads from it as
// the record itself reads. The detector intercalibration hops Ar40 onto every
// Faraday, so its isotopes are one a detector.
TEST_P(InstallE2E, EveryShippedPlanIsStoredAndReadsAsItsRecord) {
  std::vector<std::string> shipped;
  for (const auto& entry : fs::directory_iterator(site_.root / "plans"))
    if (entry.path().extension() == ".toml") shipped.push_back(entry.path().stem().string());
  std::sort(shipped.begin(), shipped.end());
  // Whatever is shipped is run; these three are, today.
  for (const char* plan : {"detector_ic", "multicollect", "multicollect_hop_ar39"})
    ASSERT_EQ(std::count(shipped.begin(), shipped.end(), plan), 1) << plan;

  // The hop plan again, with a third hop that is a baseline on the CDD.
  std::string hop_plan = read_file(site_.root / "plans" / "multicollect_hop_ar39.toml");
  const auto name_at = hop_plan.find("name = \"multicollect_hop_ar39\"");
  ASSERT_NE(name_at, std::string::npos);
  hop_plan.replace(name_at, std::string("name = \"multicollect_hop_ar39\"").size(), "name = \"hop_baseline\"");
  const auto fits_at = hop_plan.find("[fits]");
  ASSERT_NE(fits_at, std::string::npos);
  hop_plan.insert(fits_at, "[[main.hops]]\ncounts = 5\nsettle_s = 3\nbaseline = true\nmass = 34.2\n"
                           "positions = { Ar36 = \"CDD\" }\nposition = { detector = \"CDD\" }\n\n");
  std::ofstream(site_.root / "plans" / "hop_baseline.toml") << hop_plan;

  std::string text = "[queue]\nname = \"every-plan\"\nmass_spectrometer = \"argus\"\nusername = \"pychron\"\n";
  auto plans = shipped;
  plans.emplace_back("hop_baseline");
  for (const auto& plan : plans) {
    // The intercalibration under its own identifier; the others are airs.
    text += std::string("\n[[runs]]\nidentifier = \"") + (plan == "detector_ic" ? "ic" : "a") +
            "\"\nextraction = { script = \"sim_air\", duration = 30 }\nmeasurement = { plan = \"" + plan +
            "\" }\npost_measurement = \"sim_pump\"\n";
  }
  std::ofstream(site_.root / "every_plan.toml") << text;
  lab_ = lab::load_lab({site_.root, site_.path(site_.line), site_.path(site_.spectrometer)});
  ASSERT_TRUE(lab_.problems.empty()) << lab_.problems.front();
  auto queue = load_queue_file((site_.root / "every_plan.toml").string(), lab_.ids);
  ASSERT_TRUE(queue) << queue.error().what;

  const Ran ran = run_queue(*queue);
  ASSERT_TRUE(ran.result.has_value());
  EXPECT_EQ(ran.result->end, executor::QueueEnd::Completed) << ran.result->reason;
  ASSERT_EQ(ran.result->runs.size(), plans.size());
  for (const auto& r : ran.result->runs) {
    EXPECT_EQ(r.state, run::RunState::Success) << r.identifier << " " << r.error.value_or("");
    EXPECT_FALSE(r.save_error) << r.identifier;
  }
  EXPECT_EQ(ran.pending, 0u);
  ASSERT_EQ(ran.saved.size(), plans.size());

  auto opened = processing::StoreSource::open(ps::StoreConfig{url_, false});
  ASSERT_TRUE(opened) << opened.error().what;
  processing::StoreSource& source = **opened;
  auto store = ps::open_store(ps::StoreConfig{url_, false});
  ASSERT_TRUE(store) << store.error().what;

  for (std::size_t i = 0; i < plans.size(); ++i) {
    const auto& s = ran.saved[i];
    const auto& rec = s.record;
    SCOPED_TRACE(plans[i]);
    EXPECT_TRUE(s.ok) << s.error;  // the store took it
    auto loaded = source.load(rec.identity.uuid);
    ASSERT_TRUE(loaded) << loaded.error().what;
    ASSERT_NE(*loaded, nullptr);
    const auto want = processing::analysis_from_record(rec);
    expect_same_analysis(**loaded, want);
    auto raw = source.load_raw(rec.identity.uuid);
    ASSERT_TRUE(raw) << raw.error().what;
    const auto want_raw = processing::raw_from_record(rec);
    expect_same_raw(*raw, want_raw);
    expect_same_sigmas(**store, rec);
    // Every isotope's raw signal is found by the isotope's key, on both paths.
    for (const auto& iso : (*loaded)->isotopes) {
      const auto* series = raw->find(processing::SeriesKind::Signal, iso.key);
      ASSERT_NE(series, nullptr) << iso.key;
      EXPECT_EQ(series->detector, iso.detector) << iso.key;
    }

    if (plans[i] == "detector_ic") {
      EXPECT_EQ((*loaded)->analysis_type, "detector_ic");
      std::vector<std::string> keys, detectors;
      for (const auto& iso : (*loaded)->isotopes) {
        keys.push_back(iso.key);
        detectors.push_back(iso.detector);
        EXPECT_EQ(iso.isotope, "Ar40");
        EXPECT_GT(iso.intercept.value, 1000.0) << iso.key;  // an air's Ar40, on each of them
        std::printf("detector_ic %s: %.1f\n", iso.key.c_str(), iso.intercept.value);
      }
      std::sort(keys.begin(), keys.end());
      EXPECT_EQ(keys, (std::vector<std::string>{"AX:Ar40", "H1:Ar40", "L1:Ar40", "L2:Ar40"}));
      // The record itself keeps the acquisition's names.
      EXPECT_TRUE(rec.results.intercepts.contains("Ar40:H1"));
    }
    if (plans[i] == "hop_baseline") {
      std::size_t in_record = 0, points = 0;
      for (const auto& series : rec.data.series)
        if (series.kind == "baseline" && series.det == "CDD") {
          ++in_record;
          points += series.trace.t.size();
        }
      EXPECT_EQ(in_record, 2u);  // the block and the hop
      std::size_t stored = 0;
      for (const auto& series : raw->series)
        if (series.kind == processing::SeriesKind::Baseline && series.key == "CDD") {
          ++stored;
          EXPECT_EQ(series.t.size(), points);
        }
      EXPECT_EQ(stored, 1u);
    }
  }
  EXPECT_LT(std::chrono::steady_clock::now() - began_, 30s);
}

INSTANTIATE_TEST_SUITE_P(Engines, InstallE2E, ::testing::ValuesIn(ps::testing::engines()));

}  // namespace
}  // namespace pychron::experiment
