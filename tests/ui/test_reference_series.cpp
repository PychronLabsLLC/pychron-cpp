// The References dock of the experiment window
// (docs/superpowers/plans/2026-10-13-experiment-reference-series.md). Task 4:
// the pipeline of a tab, run directly on a MemorySource, no bridge. Task 5:
// the dock's widget, over a ProcessingBridge on such a source. Task 6: the
// dock in the experiment window, after a queue of the simulated lab.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <QDialog>
#include <QDockWidget>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest/QtTest>

#include "experiment_bridge.hpp"
#include "experiment_fixture.hpp"
#include "experiment_window.hpp"
#include "executor_pane.hpp"
#include "fixtures.hpp"
#include "pychron/experiment/model/queue_file.hpp"
#include "pychron/processing/record_source.hpp"
#include "options_editor.hpp"
#include "preset_bar.hpp"
#include "processing_bridge.hpp"
#include "pychron/processing/domain.hpp"
#include "pychron/processing/options.hpp"
#include "pychron/processing/source.hpp"
#include "pychron/processing/units.hpp"
#include "reference_series.hpp"
#include "reference_series_dock.hpp"
#include "scene_view.hpp"

namespace pp = pychron::processing;
using pychron::ui::analysis_types;
using pychron::ui::OptionsEditor;
using pychron::ui::PresetBar;
using pychron::ui::ProcessingBridge;
using pychron::ui::ReferenceSeriesDock;
using pychron::ui::SceneView;
using pychron::ui::kReferenceFigure;
using pychron::ui::reference_pipeline;
using pychron::ui::ReferenceKind;
using pychron::ui::ReferenceQuery;
using pychron::ui::starting_preset;

namespace {

constexpr double kT0 = 1'700'000'000.0;

const pp::UnitRegistry& reg() { return pp::UnitRegistry::builtin(); }

// An analysis of `type` on `spectrometer` at `time`; its uuid is `uuid`.
pp::AnalysisPtr analysis(const std::string& uuid, const std::string& type, double time,
                         const std::string& spectrometer = "jan") {
  auto a = pp::test::make_air(0, 295.5, type, "R");
  a->uuid = uuid;
  a->timestamp = time;
  a->mass_spectrometer = spectrometer;
  return a;
}

// The figure options of a factory preset of the time series.
pp::Options preset(const std::string& name) {
  const auto& schema = reg().find("time_series")->schema();
  for (const auto& [n, text] : schema->factory_presets)
    if (n == name) {
      auto loaded = pp::options_from_toml(schema, text);
      if (loaded) return loaded->options;
    }
  return pp::Options(schema);
}

pp::Options figure_of(ReferenceKind kind) { return preset(std::string(starting_preset(kind))); }

ReferenceQuery query(ReferenceKind kind, int last_n = 50, std::string spectrometer = "jan") {
  ReferenceQuery q;
  q.kind = kind;
  q.last_n = last_n;
  q.mass_spectrometer = std::move(spectrometer);
  return q;
}

struct Drawn {
  std::vector<std::string> uuids;  // the grouped dataset, in its order
  pp::ScenePtr scene;
  std::string error;
};

Drawn run(pp::IAnalysisSource& src, const ReferenceQuery& q, const pp::Options& figure) {
  Drawn out;
  const auto pipeline = reference_pipeline(q, figure);
  pp::Runner runner(reg(), &src);
  auto data = runner.run(pipeline, "group");
  if (!data) {
    out.error = data.error().what;
    return out;
  }
  for (const auto& item : std::get<pp::DatasetPtr>(data->at(0))->items())
    out.uuids.push_back(item.analysis->analysis->uuid);
  auto scene = runner.run(pipeline, kReferenceFigure);
  if (!scene) {
    out.error = scene.error().what;
    return out;
  }
  out.scene = std::get<pp::ScenePtr>(scene->at(0));
  return out;
}

Drawn run(pp::IAnalysisSource& src, const ReferenceQuery& q) { return run(src, q, figure_of(q.kind)); }

// The legend entries of the first panel: one a group.
std::set<std::string> group_labels(const pp::Scene& scene) {
  std::set<std::string> labels;
  if (scene.graphs.empty() || scene.graphs[0].panels.empty()) return labels;
  for (const auto& layer : scene.graphs[0].panels[0].layers)
    if (const auto* points = std::get_if<pp::PointLayer>(&layer))
      if (!points->label.empty()) labels.insert(points->label);
  return labels;
}

std::vector<std::string> strings(std::initializer_list<const char*> l) { return {l.begin(), l.end()}; }

// A MemorySource that writes down the threads that read it and how often the
// Blanks tab browsed, can hold every load until released, can fail its
// browse, and can gain an analysis at a given refresh (a store that has a run
// only some time after its RunFinished).
class DockSource : public pp::MemorySource {
 public:
  pychron::Result<void> refresh() override {
    note();
    pp::AnalysisPtr arriving;
    {
      const std::lock_guard lock(mutex_);
      ++refreshes_;
      if (late_ && refreshes_ == late_at_) arriving = std::exchange(late_, nullptr);
    }
    if (arriving) add(arriving);
    return {};
  }
  pychron::Result<pp::BrowsePage> browse(const pp::BrowseQuery& query) override {
    note();
    {
      const std::lock_guard lock(mutex_);
      if (query.analysis_types.size() > 1) ++blank_browses_;
    }
    if (fail_browse) return pychron::fail(pychron::ErrorKind::Io, "the store is gone", "store");
    return MemorySource::browse(query);
  }
  pychron::Result<pp::AnalysisPtr> load(const std::string& uuid) override {
    note();
    {
      std::unique_lock lock(mutex_);
      if (closed_) {
        ++held_;
        gate_.wait(lock, [this] { return !closed_; });
        --held_;
      }
    }
    return MemorySource::load(uuid);
  }

  void close() {
    const std::lock_guard lock(mutex_);
    closed_ = true;
  }
  void open() {
    {
      const std::lock_guard lock(mutex_);
      closed_ = false;
    }
    gate_.notify_all();
  }
  int held() const {
    const std::lock_guard lock(mutex_);
    return held_;
  }
  int refreshes() const {
    const std::lock_guard lock(mutex_);
    return refreshes_;
  }
  int blank_browses() const {
    const std::lock_guard lock(mutex_);
    return blank_browses_;
  }
  std::set<std::thread::id> threads() const {
    const std::lock_guard lock(mutex_);
    return threads_;
  }
  // `a` is in the source from the `n`th refresh from now on.
  void arrives_at_refresh(pp::AnalysisPtr a, int n) {
    const std::lock_guard lock(mutex_);
    late_ = std::move(a);
    late_at_ = refreshes_ + n;
  }

  std::atomic<bool> fail_browse{false};

 private:
  void note() {
    const std::lock_guard lock(mutex_);
    threads_.insert(std::this_thread::get_id());
  }

  mutable std::mutex mutex_;
  std::condition_variable gate_;
  bool closed_ = false;
  int held_ = 0;
  int refreshes_ = 0;
  int blank_browses_ = 0;
  std::set<std::thread::id> threads_;
  pp::AnalysisPtr late_;
  int late_at_ = 0;
};

// The Ar-Ar domain, made once for every bridge of the file.
const pychron::processing::Domain& arar_domain() {
  static const std::unique_ptr<pychron::processing::Domain> domain = pychron::processing::make_arar_domain();
  return *domain;
}

// What a dock needs. Declared in the order they must be made: the source
// before the bridge, the bridge before any dock (a test's dock is a local
// declared after its Rig).
struct Rig {
  QTemporaryDir dir;
  DockSource src;
  ProcessingBridge bridge{src, arar_domain()};
  pp::PresetStore presets{(dir.path() + QStringLiteral("/presets")).toStdString()};
  QSettings settings{dir.path() + QStringLiteral("/ui.ini"), QSettings::IniFormat};
  double wall = kT0 + 100'000.0;
  std::string spectrometer = "jan";
  // What the docks asked to have done later, and how much later.
  std::vector<std::chrono::seconds> delays;
  // Held for the test to run by hand (run_later()); false: run at once, on
  // the event loop.
  bool hold_later = false;
  std::vector<std::function<void()>> held;

  // Runs what was held; how many there were.
  std::size_t run_later() {
    auto due = std::exchange(held, {});
    for (auto& what : due) what();
    return due.size();
  }

  ReferenceSeriesDock::Deps deps() {
    ReferenceSeriesDock::Deps d;
    d.bridge = &bridge;
    d.presets = &presets;
    d.wall_now = [this] { return wall; };
    d.mass_spectrometer = [this] { return spectrometer; };
    d.settings = &settings;
    // At once, on the event loop: the test stands in for the session's clock.
    d.later = [this](std::chrono::seconds delay, std::function<void()> what) {
      delays.push_back(delay);
      if (hold_later)
        held.push_back(std::move(what));
      else
        QTimer::singleShot(0, std::move(what));
    };
    return d;
  }
  std::unique_ptr<ReferenceSeriesDock> dock() { return std::make_unique<ReferenceSeriesDock>(deps()); }
  // Reads, and waits until both tabs have delivered.
  bool read(ReferenceSeriesDock& d) {
    d.read();
    return bridge.wait_idle();
  }
};

// The points of the first panel of a view's scene, over every group.
int points(const SceneView* view) {
  const auto& scene = view->scene();
  if (!scene || scene->graphs.empty() || scene->graphs[0].panels.empty()) return 0;
  int n = 0;
  for (const auto& layer : scene->graphs[0].panels[0].layers)
    if (const auto* p = std::get_if<pp::PointLayer>(&layer)) n += static_cast<int>(p->x.size());
  return n;
}

bool says_no_analyses(const SceneView* view) {
  const auto& scene = view->scene();
  return scene && std::ranges::find(scene->warnings, std::string("no analyses")) != scene->warnings.end();
}

// The uuids of every point of a view's scene.
std::set<std::string> scene_uuids(const SceneView* view) {
  std::set<std::string> out;
  const auto& scene = view->scene();
  if (!scene) return out;
  for (const auto& graph : scene->graphs)
    for (const auto& panel : graph.panels)
      for (const auto& layer : panel.layers)
        if (const auto* p = std::get_if<pp::PointLayer>(&layer))
          for (const auto& ref : p->refs) out.insert(ref.analysis);
  return out;
}

SceneView* blanks(const ReferenceSeriesDock& d) { return d.view(ReferenceKind::Blanks); }
SceneView* airs(const ReferenceSeriesDock& d) { return d.view(ReferenceKind::Airs); }

// Two blanks and an air of the session's spectrometer, and an unknown.
void seed(DockSource& src) {
  src.add(analysis("b1", "blank_unknown", kT0 + 100));
  src.add(analysis("a1", "air", kT0 + 200));
  src.add(analysis("u1", "unknown", kT0 + 300));
  src.add(analysis("b2", "blank_air", kT0 + 400));
}

}  // namespace

class TestReferenceSeries : public QObject {
  Q_OBJECT

 private slots:
  void blanksAreTheNewestNOfEveryBlankKind() {
    const std::vector<std::string> kinds = {"blank_unknown", "blank_air", "blank_cocktail", "blank"};
    pp::MemorySource src;
    for (int i = 0; i < 7; ++i)
      src.add(analysis("b" + std::to_string(i), kinds[static_cast<std::size_t>(i) % kinds.size()], kT0 + 3600.0 * i));
    const auto drawn = run(src, query(ReferenceKind::Blanks, 5));
    QCOMPARE(drawn.error, std::string());
    QCOMPARE(drawn.uuids, strings({"b2", "b3", "b4", "b5", "b6"}));
    QVERIFY(drawn.scene);
    QCOMPARE(group_labels(*drawn.scene), (std::set<std::string>{"blank_unknown", "blank_air", "blank_cocktail", "blank"}));
  }

  void airsAreAirsOnly() {
    pp::MemorySource src;
    src.add(analysis("a1", "air", kT0));
    src.add(analysis("b1", "blank_air", kT0 + 10));
    src.add(analysis("a2", "air", kT0 + 20));
    src.add(analysis("b2", "blank_unknown", kT0 + 30));
    const auto drawn = run(src, query(ReferenceKind::Airs));
    QCOMPARE(drawn.uuids, strings({"a1", "a2"}));
    QCOMPARE(group_labels(*drawn.scene), std::set<std::string>{"air"});
  }

  void unknownsAndCocktailsAreInNeither() {
    pp::MemorySource src;
    src.add(analysis("u", "unknown", kT0));
    src.add(analysis("c", "cocktail", kT0 + 10));
    src.add(analysis("d", "detector_ic", kT0 + 15));
    src.add(analysis("b", "blank_extractionline", kT0 + 20));
    src.add(analysis("a", "air", kT0 + 30));
    QCOMPARE(run(src, query(ReferenceKind::Blanks)).uuids, strings({"b"}));
    QCOMPARE(run(src, query(ReferenceKind::Airs)).uuids, strings({"a"}));
  }

  void anotherSpectrometersAreLeftOut() {
    pp::MemorySource src;
    src.add(analysis("jan1", "blank_unknown", kT0, "jan"));
    src.add(analysis("obama1", "blank_unknown", kT0 + 10, "obama"));
    src.add(analysis("jan2", "air", kT0 + 20, "jan"));
    src.add(analysis("obama2", "air", kT0 + 30, "obama"));
    QCOMPARE(run(src, query(ReferenceKind::Blanks, 50, "jan")).uuids, strings({"jan1"}));
    QCOMPARE(run(src, query(ReferenceKind::Airs, 50, "obama")).uuids, strings({"obama2"}));
  }

  void anEmptySpectrometerMeansAny() {
    pp::MemorySource src;
    src.add(analysis("jan1", "blank_unknown", kT0, "jan"));
    src.add(analysis("obama1", "blank_unknown", kT0 + 10, "obama"));
    QCOMPARE(run(src, query(ReferenceKind::Blanks, 50, "")).uuids, strings({"jan1", "obama1"}));
  }

  void sinceLeavesOutWhatIsOlder() {
    pp::MemorySource src;
    src.add(analysis("old", "blank_unknown", kT0));
    src.add(analysis("at", "blank_unknown", kT0 + 100));
    src.add(analysis("new", "blank_air", kT0 + 200));
    auto q = query(ReferenceKind::Blanks);
    q.since = kT0 + 100;
    QCOMPARE(run(src, q).uuids, strings({"at", "new"}));
  }

  void nothingFoundIsASceneThatSaysSo() {
    pp::MemorySource src;
    src.add(analysis("u", "unknown", kT0));
    for (const auto kind : {ReferenceKind::Blanks, ReferenceKind::Airs}) {
      const auto drawn = run(src, query(kind));
      QCOMPARE(drawn.error, std::string());
      QVERIFY(drawn.uuids.empty());
      QVERIFY(drawn.scene);
      QVERIFY(std::ranges::find(drawn.scene->warnings, std::string("no analyses")) != drawn.scene->warnings.end());
    }
  }

  void theFigureOptionsReachTheFigure() {
    pp::MemorySource src;
    src.add(analysis("b", "blank_unknown", kT0));
    src.add(analysis("a", "blank_unknown", kT0 + 10));
    // Blanks draws the five isotopes, Air monitor three panels.
    const auto blanks = run(src, query(ReferenceKind::Blanks), preset("Blanks"));
    const auto air = run(src, query(ReferenceKind::Blanks), preset("Air monitor"));
    QVERIFY(blanks.scene && air.scene);
    QCOMPARE(blanks.scene->graphs.at(0).panels.size(), std::size_t{5});
    QCOMPARE(air.scene->graphs.at(0).panels.size(), std::size_t{3});
    QCOMPARE(blanks.scene->graphs.at(0).panels.at(0).quantity, std::string("Ar40.bs_corrected"));
    QCOMPARE(air.scene->graphs.at(0).panels.at(0).quantity, std::string("Ar40/Ar36"));
  }

  void theAnalysisTypesOfEachTab() {
    QCOMPARE(analysis_types(ReferenceKind::Blanks),
             strings({"blank_unknown", "blank_air", "blank_cocktail", "blank_extractionline", "blank"}));
    QCOMPARE(analysis_types(ReferenceKind::Airs), strings({"air"}));
    QCOMPARE(std::string(starting_preset(ReferenceKind::Blanks)), std::string("Blanks"));
    QCOMPARE(std::string(starting_preset(ReferenceKind::Airs)), std::string("Air monitor"));
  }

  void thePipelineIsTheFourUnitsWithTheFigureLast() {
    const auto p = reference_pipeline(query(ReferenceKind::Airs, 7), figure_of(ReferenceKind::Airs));
    QVERIFY(p.validate(reg()).has_value());
    std::vector<std::string> kinds;
    kinds.reserve(p.nodes.size());
    for (const auto& n : p.nodes) kinds.push_back(n.kind);
    QCOMPARE(kinds, strings({"select", "reduce", "group", "time_series"}));
    QCOMPARE(p.find(kReferenceFigure)->kind, std::string("time_series"));
    const auto& select = p.find("select")->options;
    QCOMPARE(select.get_int("limit"), std::int64_t{7});
    QVERIFY(select.get_strings("uuids").empty());
    QCOMPARE(select.get_strings("remove_tags"), strings({"invalid"}));
    QCOMPARE(p.find("group")->options.get_string("key"), std::string("analysis_type"));
  }

  // ---- Task 5: the dock's widget

  void showsTheBlanksAndTheAirs() {
    Rig rig;
    seed(rig.src);
    auto dock = rig.dock();
    QVERIFY(rig.read(*dock));
    QCOMPARE(points(blanks(*dock)), 2);
    QCOMPARE(points(airs(*dock)), 1);
  }

  void aFurtherBlankShowsAfterTheNextRead() {
    Rig rig;
    seed(rig.src);
    auto dock = rig.dock();
    QVERIFY(rig.read(*dock));
    rig.src.add(analysis("b3", "blank_cocktail", kT0 + 500));
    QCOMPARE(points(blanks(*dock)), 2);
    QVERIFY(rig.read(*dock));
    QCOMPARE(points(blanks(*dock)), 3);
    QCOMPARE(points(airs(*dock)), 1);
  }

  void resetEmptiesTheTabsAndTheNextReferenceReturns() {
    Rig rig;
    seed(rig.src);
    auto dock = rig.dock();
    QVERIFY(rig.read(*dock));
    rig.wall = kT0 + 5000;
    dock->reset();
    QVERIFY(rig.bridge.wait_idle());
    QVERIFY(says_no_analyses(blanks(*dock)));
    QVERIFY(says_no_analyses(airs(*dock)));
    rig.src.add(analysis("b3", "blank_unknown", kT0 + 5001));
    QVERIFY(rig.read(*dock));
    QCOMPARE(points(blanks(*dock)), 1);
    QVERIFY(says_no_analyses(airs(*dock)));
  }

  // The dock's "Last" goes down to 5, not 1: the newest five of seven.
  void lastNShowsTheNewestOnly() {
    Rig rig;
    for (int i = 0; i < 7; ++i) rig.src.add(analysis("b" + std::to_string(i), "blank_unknown", kT0 + 10.0 * i));
    auto dock = rig.dock();
    QVERIFY(rig.read(*dock));
    QCOMPARE(points(blanks(*dock)), 7);
    dock->set_last_n(5);
    QVERIFY(rig.bridge.wait_idle());
    QCOMPARE(points(blanks(*dock)), 5);
    dock->set_last_n(1);
    QCOMPARE(dock->last_n(), 5);
  }

  void lastNIsKeptInSettings() {
    Rig rig;
    const QString key = QStringLiteral("experiment_window/reference_series/last_n");
    {
      auto dock = rig.dock();
      QCOMPARE(dock->last_n(), 50);
      dock->set_last_n(42);
      QCOMPARE(rig.settings.value(key).toInt(), 42);
    }
    QCOMPARE(rig.dock()->last_n(), 42);
    rig.settings.setValue(key, 1);
    QCOMPARE(rig.dock()->last_n(), 5);
    rig.settings.setValue(key, 9999);
    QCOMPARE(rig.dock()->last_n(), 500);
    QVERIFY(rig.bridge.wait_idle());
  }

  void thePresetOfEachTabIsKept() {
    Rig rig;
    const QString blanks_key = QStringLiteral("experiment_window/reference_series/blanks/preset");
    const QString airs_key = QStringLiteral("experiment_window/reference_series/airs/preset");
    {
      auto dock = rig.dock();
      QCOMPARE(dock->preset(ReferenceKind::Blanks), QStringLiteral("Blanks"));
      QCOMPARE(dock->preset(ReferenceKind::Airs), QStringLiteral("Air monitor"));
      auto* bar = dock->options_dialog(ReferenceKind::Blanks)->findChild<PresetBar*>();
      QVERIFY(bar);
      QVERIFY(bar->select(QStringLiteral("Spectrometer")));
      QCOMPARE(dock->preset(ReferenceKind::Blanks), QStringLiteral("Spectrometer"));
      QCOMPARE(rig.settings.value(blanks_key).toString(), QStringLiteral("Spectrometer"));
      QVERIFY(!rig.settings.contains(airs_key));
      QVERIFY(rig.bridge.wait_idle());
    }
    {
      auto dock = rig.dock();
      QCOMPARE(dock->preset(ReferenceKind::Blanks), QStringLiteral("Spectrometer"));
      QCOMPARE(dock->preset(ReferenceKind::Airs), QStringLiteral("Air monitor"));
      QVERIFY(dock->figure_options(ReferenceKind::Blanks) == preset("Spectrometer"));
    }
    rig.settings.setValue(blanks_key, QStringLiteral("Gone since"));
    auto dock = rig.dock();
    QCOMPARE(dock->preset(ReferenceKind::Blanks), QStringLiteral("Blanks"));
    QVERIFY(dock->figure_options(ReferenceKind::Blanks) == preset("Blanks"));
  }

  void aFailedReadKeepsTheLastFigureAndSaysSo() {
    Rig rig;
    seed(rig.src);
    auto dock = rig.dock();
    QVERIFY(rig.read(*dock));
    QVERIFY(dock->status().startsWith(QStringLiteral("read ")));
    const auto before = blanks(*dock)->scene();
    QVERIFY(before);
    rig.src.add(analysis("b3", "blank_unknown", kT0 + 500));  // something to read
    rig.src.fail_browse = true;
    QVERIFY(rig.read(*dock));
    QCOMPARE(blanks(*dock)->scene(), before);
    QVERIFY2(dock->status().startsWith(QStringLiteral("could not read")), qPrintable(dock->status()));
    QVERIFY(dock->status().contains(QStringLiteral("the store is gone")));
  }

  void aReadWhileOneIsRunningIsFollowedByOneMore() {
    Rig rig;
    seed(rig.src);
    auto dock = rig.dock();
    rig.src.close();
    dock->read();
    QTRY_COMPARE_WITH_TIMEOUT(rig.src.held(), 1, 5000);
    rig.src.add(analysis("b3", "blank_unknown", kT0 + 500));
    dock->read();
    rig.src.add(analysis("b4", "blank_unknown", kT0 + 600));
    dock->read();
    rig.src.open();
    QVERIFY(rig.bridge.wait_idle());
    QCOMPARE(rig.src.blank_browses(), 2);
    QCOMPARE(points(blanks(*dock)), 4);
    QCOMPARE(points(airs(*dock)), 1);
  }

  void theStatusGivesTheSessionsTime() {
    Rig rig;
    seed(rig.src);
    rig.wall = 978'352'496.0;  // 2001-01-01, far from the system's clock
    auto dock = rig.dock();
    QVERIFY(rig.read(*dock));
    const QString expected =
        QDateTime::fromMSecsSinceEpoch(978'352'496'000LL).toString(QStringLiteral("HH:mm:ss"));
    QCOMPARE(dock->status(), QStringLiteral("read ") + expected);
  }

  void nothingIsReadOnTheGuiThread() {
    Rig rig;
    seed(rig.src);
    auto dock = rig.dock();
    QVERIFY(rig.read(*dock));
    QVERIFY(rig.src.refreshes() > 0);
    const auto threads = rig.src.threads();
    QVERIFY(!threads.empty());
    QVERIFY(!threads.contains(std::this_thread::get_id()));
  }

  void showingItReadsOnceAndShowingItAgainDoesNot() {
    Rig rig;
    seed(rig.src);
    auto dock = rig.dock();
    QCOMPARE(rig.src.refreshes(), 0);  // not read before it is shown
    dock->show();
    QVERIFY(rig.bridge.wait_idle());
    QCOMPARE(rig.src.refreshes(), 2);  // one a tab
    QCOMPARE(points(blanks(*dock)), 2);
    dock->hide();
    dock->show();
    QVERIFY(rig.bridge.wait_idle());
    QCOMPARE(rig.src.refreshes(), 2);
  }

  void theRefreshButtonReads() {
    Rig rig;
    seed(rig.src);
    auto dock = rig.dock();
    QVERIFY(rig.read(*dock));
    rig.src.add(analysis("b3", "blank_unknown", kT0 + 500));
    auto* button = dock->findChild<QPushButton*>(QStringLiteral("ReferenceRefresh"));
    QVERIFY(button);
    button->click();
    QVERIFY(rig.bridge.wait_idle());
    QCOMPARE(points(blanks(*dock)), 3);
  }

  void theResetButtonResets() {
    Rig rig;
    seed(rig.src);
    auto dock = rig.dock();
    QVERIFY(rig.read(*dock));
    rig.wall = kT0 + 5000;
    auto* button = dock->findChild<QPushButton*>(QStringLiteral("ReferenceReset"));
    QVERIFY(button);
    button->click();
    QVERIFY(rig.bridge.wait_idle());
    QVERIFY(says_no_analyses(blanks(*dock)));
    QVERIFY(says_no_analyses(airs(*dock)));
  }

  void aReadThatFindsNothingNewDrawsNothingAgain() {
    Rig rig;
    seed(rig.src);
    auto dock = rig.dock();
    QVERIFY(rig.read(*dock));
    const auto b = blanks(*dock)->scene();
    const auto a = airs(*dock)->scene();
    QVERIFY(rig.read(*dock));
    QCOMPARE(blanks(*dock)->scene(), b);
    QCOMPARE(airs(*dock)->scene(), a);
  }

  void theOptionsButtonOpensTheEditorAndAChangeDrawsThatTabAgain() {
    Rig rig;
    seed(rig.src);
    auto dock = rig.dock();
    QVERIFY(rig.read(*dock));
    const auto b = blanks(*dock)->scene();
    const auto a = airs(*dock)->scene();
    auto* button = dock->findChild<QToolButton*>(QStringLiteral("ReferenceBlanksOptions"));
    QVERIFY(button);
    button->click();
    auto* dialog = dock->findChild<QDialog*>(QStringLiteral("ReferenceBlanksOptionsDialog"));
    QVERIFY(dialog);
    QVERIFY(dialog->isVisible());
    QVERIFY(dialog->findChild<PresetBar*>());
    auto* editor = dialog->findChild<OptionsEditor*>();
    QVERIFY(editor);
    QVERIFY(editor->apply(QStringLiteral("x.kind"), std::string("index")));
    QVERIFY(rig.bridge.wait_idle());
    QVERIFY(blanks(*dock)->scene() != b);
    QCOMPARE(blanks(*dock)->scene()->graphs[0].x.format, pp::AxisFormat::Number);
    QCOMPARE(airs(*dock)->scene(), a);
    QCOMPARE(dock->figure_options(ReferenceKind::Blanks).get_string("x.kind"), std::string("index"));
    dialog->close();
  }

  void theResetPointIsNotKept() {
    Rig rig;
    seed(rig.src);
    {
      auto dock = rig.dock();
      rig.wall = kT0 + 5000;
      dock->reset();
      QVERIFY(rig.bridge.wait_idle());
      QVERIFY(says_no_analyses(blanks(*dock)));
    }
    auto dock = rig.dock();
    QVERIFY(rig.read(*dock));
    QCOMPARE(points(blanks(*dock)), 2);
    QCOMPARE(points(airs(*dock)), 1);
  }

  void aRunTheStoreDoesNotHaveYetIsReadForAgain() {
    Rig rig;
    seed(rig.src);
    auto dock = rig.dock();
    QVERIFY(rig.read(*dock));
    // The run's RunFinished comes first; the store has it from the read
    // after the next (two refreshes a read).
    rig.src.arrives_at_refresh(analysis("b3", "blank_unknown", kT0 + 500), 3);
    dock->run_saved();
    QTRY_COMPARE_WITH_TIMEOUT(points(blanks(*dock)), 3, 5000);
    QVERIFY(rig.bridge.wait_idle());
    QCOMPARE(rig.delays, (std::vector<std::chrono::seconds>{std::chrono::seconds(2)}));
  }

  void aSavedRunThatNeverArrivesIsReadForTwiceMoreThenNot() {
    Rig rig;
    rig.hold_later = true;
    seed(rig.src);
    auto dock = rig.dock();
    QVERIFY(rig.read(*dock));
    const int before = rig.src.refreshes();
    dock->run_saved();
    QVERIFY(rig.bridge.wait_idle());
    // 2 s after the run, then 10 s after it: 2 s and 8 s more.
    QCOMPARE(rig.delays, (std::vector<std::chrono::seconds>{std::chrono::seconds(2)}));
    QCOMPARE(rig.run_later(), std::size_t{1});
    QVERIFY(rig.bridge.wait_idle());
    QCOMPARE(rig.delays, (std::vector<std::chrono::seconds>{std::chrono::seconds(2), std::chrono::seconds(8)}));
    QCOMPARE(rig.run_later(), std::size_t{1});
    QVERIFY(rig.bridge.wait_idle());
    QCOMPARE(rig.delays.size(), std::size_t{2});
    QVERIFY(rig.held.empty());
    QCOMPARE(rig.src.refreshes() - before, 6);  // three reads of two tabs
  }

  void aNewerSavedRunReplacesTheReadsAgainOfTheOlder() {
    Rig rig;
    rig.hold_later = true;
    seed(rig.src);
    auto dock = rig.dock();
    QVERIFY(rig.read(*dock));
    dock->run_saved();
    QVERIFY(rig.bridge.wait_idle());
    dock->run_saved();
    QVERIFY(rig.bridge.wait_idle());
    QCOMPARE(rig.held.size(), std::size_t{2});
    const int before = rig.src.refreshes();
    rig.held.front()();  // the older run's: nothing
    QVERIFY(rig.bridge.wait_idle());
    QCOMPARE(rig.src.refreshes(), before);
    rig.held.back()();
    QVERIFY(rig.bridge.wait_idle());
    QCOMPARE(rig.src.refreshes(), before + 2);
  }

  void aSavedRunThatIsFoundAtOnceIsNotReadForAgain() {
    Rig rig;
    seed(rig.src);
    auto dock = rig.dock();
    QVERIFY(rig.read(*dock));
    rig.src.add(analysis("b3", "blank_unknown", kT0 + 500));
    dock->run_saved();
    QVERIFY(rig.bridge.wait_idle());
    QCOMPARE(points(blanks(*dock)), 3);
    QVERIFY(rig.delays.empty());
  }

  // ---- in the experiment window (Task 6; the spec's window test, section 8)

  // A queue of blank, air, unknown, blank on the simulated lab, whose record
  // files are the source (no UI fixture has a store): the dock follows it with
  // no call from the test, and a fifth run, a blank, adds a point the same way.
  void theQueueOfBlankAirUnknownBlank() {
    namespace fs = std::filesystem;
    pychron::ui::test::SimLab sim;
    QTemporaryDir dir;
    // The folder the session writes its records to.
    pp::RecordDirectorySource records(sim.dir / "data" / "records");
    pp::PresetStore presets((dir.path() + QStringLiteral("/presets")).toStdString());
    ProcessingBridge processing(records, arar_domain());
    pychron::ui::ExperimentBridge bridge(*sim.session, sim.line->bus());
    pychron::ui::ExperimentWindow window(
        bridge, true, std::make_unique<QSettings>(dir.path() + QStringLiteral("/ui.ini"), QSettings::IniFormat));
    window.set_references(&processing, &presets);
    ReferenceSeriesDock* dock = window.references();
    QVERIFY(dock != nullptr);

    // By hand: no example queue has this order. Without embedded Python the
    // scripts are dropped; the air run is still typed air by its identifier.
    auto queue = sim.queue();
    queue.delays.before_analyses = pychron::experiment::Duration{0};
    queue.delays.between_analyses = pychron::experiment::Duration{0};
    queue.delays.after_blank = pychron::experiment::Duration{0};
    const auto blank = queue.runs.at(0);
    const auto unknown = queue.runs.at(1);
    QCOMPARE(QString::fromStdString(blank.id.identifier), QStringLiteral("bu"));
    QCOMPARE(QString::fromStdString(unknown.id.identifier), QStringLiteral("66001"));
    auto air = blank;
    air.id.identifier = "a";
    air.id.type = pychron::experiment::AnalysisType::Air;
    queue.runs = {blank, air, unknown, blank};
    const fs::path file = sim.dir / "references.toml";
    QVERIFY(pychron::experiment::save_queue_file(file.string(), queue));
    QString error;
    QVERIFY2(window.load_queue(file, &error), qPrintable(error));
    window.show();

    auto* pane = window.executor();
    pane->request_start();
    QVERIFY(pane->running());
    QTRY_VERIFY_WITH_TIMEOUT(!pane->running(), 120000);
    using pychron::experiment::run::RunState;
    for (int row = 0; row < 4; ++row) QCOMPARE(window.model().status(row), std::optional<RunState>(RunState::Success));
    QTRY_COMPARE_WITH_TIMEOUT(points(blanks(*dock)), 2, 20000);
    QTRY_COMPARE_WITH_TIMEOUT(points(airs(*dock)), 1, 20000);

    // The unknown was saved, and is in neither tab.
    QVERIFY(records.refresh());
    pp::BrowseQuery unknowns;
    unknowns.analysis_types = {"unknown"};
    const auto found = records.browse(unknowns);
    QVERIFY(found);
    QCOMPARE(found->rows.size(), std::size_t{1});
    const std::string unknown_uuid = found->rows.front().uuid;
    QVERIFY(!scene_uuids(blanks(*dock)).contains(unknown_uuid));
    QVERIFY(!scene_uuids(airs(*dock)).contains(unknown_uuid));

    // A fifth run, a blank.
    auto fifth = queue;
    fifth.runs = {blank};
    const fs::path again = sim.dir / "fifth.toml";
    QVERIFY(pychron::experiment::save_queue_file(again.string(), fifth));
    QVERIFY2(window.load_queue(again, &error), qPrintable(error));
    pane->request_start();
    QVERIFY(pane->running());
    QTRY_VERIFY_WITH_TIMEOUT(!pane->running(), 60000);
    QTRY_COMPARE_WITH_TIMEOUT(points(blanks(*dock)), 3, 20000);
    QCOMPARE(points(airs(*dock)), 1);
    window.set_references(nullptr, nullptr);  // before the bridge it reads through
  }

  void destroyingItWithAReadInFlightIsSafe() {
    Rig rig;
    seed(rig.src);
    auto dock = rig.dock();
    int finished = 0;
    QObject::connect(dock.get(), &ReferenceSeriesDock::read_finished, &rig.bridge, [&finished] { ++finished; });
    rig.src.close();
    dock->read();
    QTRY_COMPARE_WITH_TIMEOUT(rig.src.held(), 1, 5000);
    dock.reset();
    rig.src.open();
    QVERIFY(rig.bridge.wait_idle());
    QCoreApplication::processEvents();
    QCOMPARE(finished, 0);
  }
};

QTEST_MAIN(TestReferenceSeries)
#include "test_reference_series.moc"
