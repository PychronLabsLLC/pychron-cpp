// ProcessingBridge's submit options (experiment reference series plan, Task
// 3): the source brought up to date on the worker before a run that asks for
// it, and the run's time handed to the units.

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <QtTest/QtTest>

#include "fixtures.hpp"
#include "processing_bridge.hpp"
#include "pychron/processing/domain.hpp"
#include "pychron/processing/source.hpp"
#include "pychron/processing/units.hpp"

namespace pp = pychron::processing;
using pychron::ui::PipelineResult;
using pychron::ui::ProcessingBridge;
using pychron::ui::SubmitOptions;

namespace {

// A MemorySource that writes down what is asked of it, and on which thread.
class LoggingSource : public pp::MemorySource {
 public:
  struct Call {
    std::string what;
    std::thread::id thread;
  };

  pychron::Result<void> refresh() override {
    note("refresh");
    if (fail_refresh) return pychron::fail(pychron::ErrorKind::Io, "the store is gone", "store");
    return {};
  }
  pychron::Result<pp::BrowsePage> browse(const pp::BrowseQuery& query) override {
    note("browse");
    return MemorySource::browse(query);
  }
  pychron::Result<pp::AnalysisPtr> load(const std::string& uuid) override {
    note("load");
    return MemorySource::load(uuid);
  }

  std::vector<Call> calls() const {
    const std::lock_guard lock(log_mutex_);
    return calls_;
  }
  int count(const std::string& what) const {
    const std::lock_guard lock(log_mutex_);
    return static_cast<int>(std::ranges::count(calls_, what, &Call::what));
  }

  bool fail_refresh = false;  // set before a submit

 private:
  void note(std::string what) {
    const std::lock_guard lock(log_mutex_);
    calls_.push_back({std::move(what), std::this_thread::get_id()});
  }

  mutable std::mutex log_mutex_;
  std::vector<Call> calls_;
};

const pp::UnitRegistry& reg() { return pp::UnitRegistry::builtin(); }

// Six airs a figure of Ar40 against hours from now.
pp::Pipeline now_pipeline() {
  pp::Pipeline p;
  auto& sel = p.add(reg(), "select", "select");
  [&] { QVERIFY(sel.options.set("analysis_types", std::vector<std::string>{"air"})); }();
  p.add(reg(), "reduce", "reduce", {"select"});
  p.add(reg(), "group", "group", {"reduce"});
  p.add(reg(), "edits", "edits", {"group"});
  auto& fig = p.add(reg(), "figure", "time_series", {"edits"});
  auto row = fig.options.new_row("panels");
  [&] {
    QVERIFY(row.set("quantity", std::string("Ar40")));
    QVERIFY(fig.options.set_rows("panels", {row}));
    QVERIFY(fig.options.set("x.kind", std::string("relative")));
    QVERIFY(fig.options.set("x.origin", std::string("now")));
  }();
  return p;
}

void fill(LoggingSource& src) {
  for (int i = 0; i < 6; ++i) src.add(pp::test::make_air(i, 295.0 + i));
}

double first_x(const PipelineResult& r) {
  if (r.outputs.empty() || !r.outputs[0]) return 0;
  const auto& scene = *std::get<pp::ScenePtr>(r.outputs[0]->at(0));
  for (const auto& layer : scene.graphs.at(0).panels.at(0).layers)
    if (const auto* points = std::get_if<pp::PointLayer>(&layer)) return points->x.at(0);
  return 0;
}

SubmitOptions refreshing() {
  SubmitOptions o;
  o.refresh_source = true;
  return o;
}

constexpr double kFirstAir = 1'700'000'000.0;  // make_air's first timestamp

// The Ar-Ar domain, made once for every bridge of the file.
const pychron::processing::Domain& arar_domain() {
  static const std::unique_ptr<pychron::processing::Domain> domain = pychron::processing::make_arar_domain();
  return *domain;
}

}  // namespace

class TestProcessingBridge : public QObject {
  Q_OBJECT

 private slots:
  void refreshIsCalledOnTheWorkerBeforeTheRun() {
    LoggingSource src;
    fill(src);
    ProcessingBridge bridge(src, arar_domain());
    std::optional<PipelineResult> got;
    bridge.submit(bridge.new_channel(), now_pipeline(), {"figure"}, [&](const PipelineResult& r) { got = r; },
                  refreshing());
    QVERIFY(bridge.wait_idle());
    QVERIFY(got.has_value());
    QVERIFY(got->outputs.at(0).has_value());
    const auto calls = src.calls();
    QVERIFY(calls.size() >= 2);
    QCOMPARE(calls[0].what, std::string("refresh"));
    QCOMPARE(calls[1].what, std::string("browse"));
    QVERIFY(calls[0].thread != std::this_thread::get_id());
    QCOMPARE(calls[0].thread, calls[1].thread);
  }

  void withoutTheOptionRefreshIsNotCalled() {
    LoggingSource src;
    fill(src);
    ProcessingBridge bridge(src, arar_domain());
    bool done = false;
    bridge.submit(bridge.new_channel(), now_pipeline(), {"figure"}, [&](const PipelineResult&) { done = true; });
    QVERIFY(bridge.wait_idle());
    QVERIFY(done);
    QCOMPARE(src.count("refresh"), 0);
    QVERIFY(src.count("browse") > 0);
  }

  void aFailedRefreshIsDeliveredAsTheRunsError() {
    LoggingSource src;
    fill(src);
    src.fail_refresh = true;
    ProcessingBridge bridge(src, arar_domain());
    std::optional<PipelineResult> got;
    bridge.submit(bridge.new_channel(), now_pipeline(), {"figure", "edits"},
                  [&](const PipelineResult& r) { got = r; }, refreshing());
    QVERIFY(bridge.wait_idle());
    QVERIFY(got.has_value());
    QCOMPARE(got->outputs.size(), std::size_t{2});
    for (const auto& out : got->outputs) {
      QVERIFY(!out.has_value());
      QCOMPARE(out.error().what, std::string("the store is gone"));
    }
    QCOMPARE(src.count("browse"), 0);
  }

  void theRunsTimeReachesTheUnits() {
    LoggingSource src;
    fill(src);
    ProcessingBridge bridge(src, arar_domain());
    const int channel = bridge.new_channel();
    std::vector<double> x;
    SubmitOptions first;
    first.now = kFirstAir + 1000 * 3600.0;
    bridge.submit(channel, now_pipeline(), {"figure"}, [&](const PipelineResult& r) { x.push_back(first_x(r)); },
                  first);
    QVERIFY(bridge.wait_idle());
    SubmitOptions later;
    later.now = kFirstAir + 2000 * 3600.0;
    bridge.submit(channel, now_pipeline(), {"figure"}, [&](const PipelineResult& r) { x.push_back(first_x(r)); },
                  later);
    QVERIFY(bridge.wait_idle());
    QCOMPARE(x, (std::vector<double>{-1000.0, -2000.0}));
  }

  void anUnchangedSourceIsNotReadAgain() {
    LoggingSource src;
    fill(src);
    ProcessingBridge bridge(src, arar_domain());
    const int channel = bridge.new_channel();
    SubmitOptions o = refreshing();
    o.now = kFirstAir;
    bridge.submit(channel, now_pipeline(), {"figure"}, [](const PipelineResult&) {}, o);
    QVERIFY(bridge.wait_idle());
    const int loads = src.count("load");
    QCOMPARE(loads, 6);
    bridge.submit(channel, now_pipeline(), {"figure"}, [](const PipelineResult&) {}, o);
    QVERIFY(bridge.wait_idle());
    QCOMPARE(src.count("refresh"), 2);
    QCOMPARE(src.count("load"), loads);
  }
};

QTEST_MAIN(TestProcessingBridge)
#include "test_processing_bridge.moc"
