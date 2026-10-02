// MeasurementPanel against the example lab plus a second plan of another
// instrument family, editing rows of the example queue. No hardware.

#include <filesystem>
#include <fstream>
#include <memory>

#include <QtTest/QtTest>

#include "experiment_fixture.hpp"
#include "measurement_panel.hpp"
#include "queue_table_model.hpp"

using pychron::experiment::ParamValue;
using pychron::ui::MeasurementPanel;
using pychron::ui::QueueTableModel;
namespace lab = pychron::experiment::lab;

namespace {

constexpr const char* kArgusPlan = R"([plan]
name = "argus_hop"
instrument_family = "argus"
description = "One-hop test plan"
analysis_types = ["unknown"]

[detectors]
reference = "H1"

[equilibration]
inlet = "@valves.inlet"
outlet = "@valves.outlet"
time_s = 20

[main]
cycles = 3
integration_s = 1

[[main.hops]]
positions = { Ar40 = "H1" }
counts = 10

[parameters]
expose = [{ path = "main.cycles", label = "Cycles" }]
)";

}  // namespace

class TestMeasurementPanel : public QObject {
  Q_OBJECT

  std::filesystem::path dir_;
  std::unique_ptr<lab::Lab> lab_;
  std::unique_ptr<QueueTableModel> model_;
  std::vector<std::size_t> selection_;
  std::unique_ptr<MeasurementPanel> panel_;

  const pychron::experiment::MeasurementRef& measurement(std::size_t row) const {
    return model_->queue().runs[row].measurement;
  }
  void select(std::vector<std::size_t> rows) {
    selection_ = std::move(rows);
    panel_->refresh();
  }

 private slots:
  void initTestCase() {
    dir_ = pychron::ui::test::scratch_lab();
    std::ofstream(dir_ / "plans" / "argus_hop.toml") << kArgusPlan;
    lab_ = std::make_unique<lab::Lab>(lab::load_lab(pychron::ui::test::lab_paths(dir_)));
    QVERIFY2(lab_->problems.empty(), lab_->problems.empty() ? "" : lab_->problems.front().c_str());
  }
  void cleanupTestCase() {
    lab_.reset();
    std::filesystem::remove_all(dir_);
  }
  void init() {
    model_ = std::make_unique<QueueTableModel>(lab_->ids, [this](const auto& q) { return lab::check_lab_queue(*lab_, q); });
    model_->set_queue(pychron::ui::test::example_queue(dir_, lab_->ids));
    selection_.clear();
    panel_ = std::make_unique<MeasurementPanel>(*lab_, *model_, [this] { return selection_; });
  }
  void cleanup() {
    panel_.reset();
    model_.reset();
  }

  void showsNothingWithoutOneSelectedRow() {
    QVERIFY(!panel_->row());
    QVERIFY(panel_->header_text().contains(QStringLiteral("Select one run")));
    QVERIFY(panel_->parameter_paths().isEmpty());
    QVERIFY(!panel_->set_parameter(QStringLiteral("main.cycles"), QStringLiteral("4")));
    select({0, 1});
    QVERIFY(!panel_->row());
  }

  void aRowShowsItsPlanAndParameters() {
    select({1});
    QCOMPARE(panel_->row(), std::optional<std::size_t>(1));
    QCOMPARE(panel_->header_text(), QStringLiteral("Row 1 · 66001 (unknown)"));
    QCOMPARE(panel_->families(), (QStringList{QString(), QStringLiteral("argus"), QStringLiteral("sim")}));
    QCOMPARE(panel_->plans(), (QStringList{QStringLiteral("argus_hop"), QStringLiteral("sim_multicollect")}));
    QCOMPARE(panel_->parameter_paths(),
             (QStringList{QStringLiteral("main.cycles"), QStringLiteral("main.hops[0].counts"),
                          QStringLiteral("main.hops[1].counts"), QStringLiteral("baseline.counts")}));
    QCOMPARE(panel_->parameter_text(QStringLiteral("main.cycles")), QStringLiteral("2"));
    QVERIFY(!panel_->overridden(QStringLiteral("main.cycles")));
    QVERIFY(panel_->status_ok());
    QVERIFY(panel_->status_text().startsWith(QStringLiteral("Measures ")));

    // Row 2 carries an override from the queue file.
    select({2});
    QVERIFY(panel_->overridden(QStringLiteral("main.cycles")));
    QCOMPARE(panel_->parameter_text(QStringLiteral("main.cycles")), QStringLiteral("1"));
  }

  void editingOverridesAndResets() {
    select({1});
    QVERIFY(panel_->set_parameter(QStringLiteral("main.cycles"), QStringLiteral("4")));
    QVERIFY(panel_->overridden(QStringLiteral("main.cycles")));
    QCOMPARE(measurement(1).overrides.at("main.cycles"), ParamValue(std::int64_t{4}));
    QVERIFY(panel_->status_text().contains(QStringLiteral("1 override(s)")));
    QCOMPARE(panel_->row(), std::optional<std::size_t>(1));  // the edit kept the row

    // Text that does not fit is refused and the editor reverts.
    QVERIFY(!panel_->set_parameter(QStringLiteral("main.cycles"), QStringLiteral("many")));
    QCOMPARE(panel_->parameter_text(QStringLiteral("main.cycles")), QStringLiteral("4"));
    QCOMPARE(measurement(1).overrides.at("main.cycles"), ParamValue(std::int64_t{4}));

    // Typing the plan's own value drops the override.
    QVERIFY(panel_->set_parameter(QStringLiteral("main.cycles"), QStringLiteral("2")));
    QVERIFY(!panel_->overridden(QStringLiteral("main.cycles")));
    QVERIFY(measurement(1).overrides.empty());

    QVERIFY(panel_->set_parameter(QStringLiteral("baseline.counts"), QStringLiteral("30")));
    QVERIFY(panel_->set_parameter(QStringLiteral("main.hops[0].counts"), QStringLiteral("20")));
    QCOMPARE(measurement(1).overrides.size(), std::size_t{2});
    QVERIFY(panel_->reset_parameter(QStringLiteral("baseline.counts")));
    QCOMPARE(measurement(1).overrides.size(), std::size_t{1});
    QVERIFY(panel_->reset_all());
    QVERIFY(measurement(1).overrides.empty());
    QVERIFY(!panel_->reset_all());
    QVERIFY(model_->runnable());
  }

  void familyFilterAndChangingThePlan() {
    select({2});
    QVERIFY(panel_->set_family(QStringLiteral("argus")));
    // The row's own plan stays first even though the filter hides it.
    QCOMPARE(panel_->plans(), (QStringList{QStringLiteral("sim_multicollect"), QStringLiteral("argus_hop")}));
    QVERIFY(panel_->choose_plan(QStringLiteral("argus_hop")));
    QCOMPARE(measurement(2).plan, std::string("argus_hop"));
    // main.cycles is a parameter of argus_hop too, so the override moves with the run.
    QCOMPARE(measurement(2).overrides.at("main.cycles"), ParamValue(std::int64_t{1}));
    QCOMPARE(panel_->parameter_paths(), QStringList{QStringLiteral("main.cycles")});
    QVERIFY(panel_->status_ok());
    QVERIFY(!panel_->choose_plan(QStringLiteral("no_such_plan")));
    QVERIFY(panel_->choose_plan(QString()));  // no plan
    QVERIFY(measurement(2).plan.empty());
    QVERIFY(measurement(2).overrides.empty());
    QVERIFY(!panel_->status_ok());
    QVERIFY(model_->row_has_error(2));
  }

  void advancedOverridesAnyValue() {
    select({1});
    QVERIFY(!panel_->parameter_paths().contains(QStringLiteral("equilibration.time_s")));
    QVERIFY(panel_->set_advanced(true));
    QVERIFY(measurement(1).advanced);
    QVERIFY(panel_->parameter_paths().contains(QStringLiteral("equilibration.time_s")));
    QVERIFY(panel_->parameter_paths().contains(QStringLiteral("sniff.enabled")));
    QVERIFY(panel_->set_parameter(QStringLiteral("equilibration.time_s"), QStringLiteral("25")));
    QVERIFY(panel_->set_parameter(QStringLiteral("sniff.enabled"), QStringLiteral("false")));
    QCOMPARE(panel_->parameter_text(QStringLiteral("sniff.enabled")), QStringLiteral("false"));
    QCOMPARE(measurement(1).overrides.at("equilibration.time_s"), ParamValue(std::int64_t{25}));
    QCOMPARE(measurement(1).overrides.at("sniff.enabled"), ParamValue(false));
    QVERIFY(panel_->status_ok());
    QVERIFY(model_->runnable());

    // Leaving Advanced keeps the overrides, which no longer load: listed, flagged, resettable.
    QVERIFY(panel_->set_advanced(false));
    QVERIFY(!panel_->status_ok());
    QVERIFY(panel_->status_text().contains(QStringLiteral("not exposed")));
    QVERIFY(panel_->parameter_paths().contains(QStringLiteral("sniff.enabled")));
    QVERIFY(model_->row_has_error(1));
    QVERIFY(panel_->reset_all());
    QVERIFY(panel_->status_ok());
    QVERIFY(!model_->row_has_error(1));
  }

  void lockedRefusesEdits() {
    select({1});
    panel_->set_locked(true);
    QVERIFY(!panel_->set_parameter(QStringLiteral("main.cycles"), QStringLiteral("4")));
    QVERIFY(!panel_->choose_plan(QStringLiteral("argus_hop")));
    QVERIFY(!panel_->set_advanced(true));
    QVERIFY(measurement(1).overrides.empty());
    panel_->set_locked(false);
    model_->set_locked(true);  // a running queue
    QVERIFY(!panel_->set_parameter(QStringLiteral("main.cycles"), QStringLiteral("4")));
    model_->set_locked(false);
    QVERIFY(panel_->set_parameter(QStringLiteral("main.cycles"), QStringLiteral("4")));
  }

  void followsEditsFromElsewhere() {
    select({1});
    auto run = model_->queue().runs[1];
    run.measurement.overrides["main.cycles"] = std::int64_t{7};
    QVERIFY(model_->replace_run(1, run));
    QCOMPARE(panel_->parameter_text(QStringLiteral("main.cycles")), QStringLiteral("7"));
    QVERIFY(panel_->overridden(QStringLiteral("main.cycles")));
  }
};

QTEST_MAIN(TestMeasurementPanel)
#include "test_measurement_panel.moc"
