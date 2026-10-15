// The References dock of the experiment window
// (docs/superpowers/plans/2026-10-13-experiment-reference-series.md). Task 4:
// the pipeline of a tab, run directly on a MemorySource, no bridge.

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <QtTest/QtTest>

#include "fixtures.hpp"
#include "pychron/processing/options.hpp"
#include "pychron/processing/source.hpp"
#include "pychron/processing/units.hpp"
#include "reference_series.hpp"

namespace pp = pychron::processing;
using pychron::ui::analysis_types;
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
};

QTEST_GUILESS_MAIN(TestReferenceSeries)
#include "test_reference_series.moc"
