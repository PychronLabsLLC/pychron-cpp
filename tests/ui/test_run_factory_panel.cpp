// RunFactoryPanel against the example lab (its defaults.toml and blocks/) and a
// QueueTableModel holding the example queue. No hardware.

#include <filesystem>
#include <fstream>
#include <memory>

#include <QLineEdit>
#include <QtTest/QtTest>

#include "experiment_fixture.hpp"
#include "queue_table_model.hpp"
#include "run_factory_panel.hpp"

using pychron::experiment::AnalysisType;
using pychron::experiment::FactoryForm;
using pychron::ui::QueueTableModel;
using pychron::ui::RunFactoryPanel;
namespace lab = pychron::experiment::lab;

class TestRunFactoryPanel : public QObject {
  Q_OBJECT

  std::filesystem::path dir_;
  std::unique_ptr<lab::Lab> lab_;
  std::unique_ptr<QueueTableModel> model_;
  std::vector<std::size_t> selection_;
  std::unique_ptr<RunFactoryPanel> panel_;
  std::vector<std::vector<std::size_t>> inserted_;

  QString identifier(int row) const {
    return QString::fromStdString(model_->queue().runs[static_cast<std::size_t>(row)].id.identifier);
  }
  FactoryForm with_identifier(const char* id, const char* position = "") const {
    FactoryForm f = panel_->form();
    f.identifier = id;
    f.position = position;
    return f;
  }

 private slots:
  void initTestCase() {
    dir_ = pychron::ui::test::scratch_lab();
    lab_ = std::make_unique<lab::Lab>(lab::load_lab(pychron::ui::test::lab_paths(dir_)));
    QVERIFY(lab_->problems.empty());
  }
  void cleanupTestCase() {
    lab_.reset();
    std::filesystem::remove_all(dir_);
  }
  void init() {
    model_ = std::make_unique<QueueTableModel>(lab_->ids, [this](const auto& q) { return lab::check_lab_queue(*lab_, q); });
    model_->set_queue(pychron::ui::test::example_queue(dir_, lab_->ids));
    selection_.clear();
    inserted_.clear();
    panel_ = std::make_unique<RunFactoryPanel>(*lab_, *model_, [this] { return selection_; });
    connect(panel_.get(), &RunFactoryPanel::inserted, this,
            [this](const std::vector<std::size_t>& rows) { inserted_.push_back(rows); });
  }
  void cleanup() {
    panel_.reset();
    model_.reset();
  }

  void startsFromTheLabsDefaultsAndChoices() {
    const FactoryForm f = panel_->form();
    QCOMPARE(f.plan, std::string("sim_multicollect"));
    QCOMPARE(f.script, std::string("sim_extract"));
    QCOMPARE(f.post_measurement, std::string("sim_pump"));
    QCOMPARE(f.value, 5.0);
    QCOMPARE(panel_->plan_choices(), QStringList{QStringLiteral("sim_multicollect")});
    QCOMPARE(panel_->script_choices(), QStringList{QStringLiteral("sim_extract")});
    QCOMPARE(panel_->block_choices(), QStringList{QStringLiteral("blank_pair")});
    // No identifier yet: nothing to add.
    QVERIFY(!panel_->add_enabled());
    QVERIFY(panel_->preview_text().contains(QStringLiteral("identifier")));
    QVERIFY(!panel_->add());
  }

  void addInsertsAfterTheSelectionAndAdvances() {
    panel_->set_form(with_identifier("20001", "1"));
    QVERIFY(panel_->add_enabled());
    QCOMPARE(panel_->preview_text(), QStringLiteral("Adds 1 run(s): 20001 @1"));
    panel_->set_increment(1, 1);
    selection_ = {0};
    QVERIFY(panel_->add());
    QCOMPARE(model_->rowCount(), 4);
    QCOMPARE(identifier(1), QStringLiteral("20001"));
    QCOMPARE(model_->queue().runs[1].measurement.plan, std::string("sim_multicollect"));
    QCOMPARE(inserted_.size(), std::size_t{1});
    QCOMPARE(inserted_.front(), std::vector<std::size_t>{1});
    QCOMPARE(panel_->form().identifier, std::string("20002"));
    QCOMPARE(panel_->form().position, std::string("2"));
    QVERIFY(model_->runnable());

    // Without a selection (or with "after selection" off) Add appends.
    selection_.clear();
    QVERIFY(panel_->add());
    QCOMPARE(identifier(4), QStringLiteral("20002"));
    selection_ = {0};
    panel_->set_insert_after_selection(false);
    QVERIFY(panel_->add());
    QCOMPARE(identifier(5), QStringLiteral("20003"));
  }

  void holesAndStepHeatsPreviewAndExpand() {
    FactoryForm f = with_identifier("20001", "1-3");
    f.identifier_step = 1;
    panel_->set_form(f);
    QCOMPARE(panel_->preview_text(), QStringLiteral("Adds 3 run(s): 20001 @1 … 20003 @3"));
    QVERIFY(panel_->add());
    QCOMPARE(model_->rowCount(), 6);
    QCOMPARE(identifier(5), QStringLiteral("20003"));
    QCOMPARE(inserted_.back(), (std::vector<std::size_t>{3, 4, 5}));

    f = with_identifier("20010", "7");
    f.step_heat = "2:2:4";
    panel_->set_form(f);
    QVERIFY(panel_->preview_text().startsWith(QStringLiteral("Adds 4 run(s): 20010A @7")));
    f.step_heat = "2, warm";
    panel_->set_form(f);
    QVERIFY(!panel_->add_enabled());
    QVERIFY(panel_->preview_text().contains(QStringLiteral("not a number")));
    QVERIFY(!panel_->add());
    QCOMPARE(model_->rowCount(), 6);
  }

  void runsTheLabWouldRejectCannotBeAdded() {
    FactoryForm f = with_identifier("20001", "1");
    f.plan = "no_such_plan";
    panel_->set_form(f);
    QVERIFY(!panel_->add_enabled());
    QVERIFY(panel_->preview_text().contains(QStringLiteral("no_such_plan")));
    f.plan = "sim_multicollect";
    f.script = "no_such_script";
    panel_->set_form(f);
    QVERIFY(!panel_->add_enabled());
    QVERIFY(panel_->preview_text().contains(QStringLiteral("no_such_script")));
  }

  void fieldsFollowTheTypeAndATypeChangeAppliesDefaults() {
    panel_->set_form(with_identifier("20001", "1"));
    QVERIFY(panel_->field_enabled("value"));
    QVERIFY(panel_->field_enabled("position"));
    // Typing a special identifier switches to its type and its defaults.
    auto* edit = panel_->findChild<QLineEdit*>(QStringLiteral("identifier"));
    QVERIFY(edit != nullptr);
    edit->setText(QString());
    QTest::keyClicks(edit, QStringLiteral("a"));
    QCOMPARE(panel_->form().identifier, std::string("a"));
    QVERIFY(!panel_->field_enabled("value"));
    QVERIFY(!panel_->field_enabled("step_heat"));
    QVERIFY(!panel_->field_enabled("position"));
    QVERIFY(panel_->field_enabled("plan"));
    QVERIFY(panel_->field_enabled("script"));
    QCOMPARE(panel_->form().post_measurement, std::string("sim_pump"));
    QCOMPARE(panel_->form().script, std::string(""));  // air's defaults have no script
    QVERIFY(panel_->add());
    const auto& added = model_->queue().runs.back();
    QCOMPARE(added.id.type, AnalysisType::Air);
    QVERIFY(!added.extraction.position.has_value());

    // No defaults for detector_ic: Defaults says so and changes nothing.
    panel_->set_form(with_identifier("ic"));
    const FactoryForm before = panel_->form();
    QVERIFY(!panel_->apply_defaults());
    QVERIFY(panel_->preview_text().contains(QStringLiteral("No defaults")));
    QCOMPARE(panel_->form(), before);
  }

  void conditionalsAreChosenFromTheLabsFiles() {
    QCOMPARE(panel_->conditional_choices(), (QStringList{QStringLiteral("system"), QStringLiteral("default_unknown")}));
    QVERIFY(panel_->form().conditionals.empty());
    QCOMPARE(panel_->conditionals_text(), QStringLiteral("(none)"));
    panel_->set_conditional_checked(QStringLiteral("default_unknown"), true);
    QCOMPARE(panel_->form().conditionals, std::vector<std::string>{"default_unknown"});
    QCOMPARE(panel_->conditionals_text(), QStringLiteral("default_unknown"));

    FactoryForm f = with_identifier("20001", "1");
    f.conditionals.clear();
    panel_->set_form(f);  // the form's list replaces the ticks
    QVERIFY(panel_->form().conditionals.empty());
    QCOMPARE(panel_->conditionals_text(), QStringLiteral("(none)"));
    f.conditionals = {"default_unknown", "gone"};  // one the lab no longer has: kept, shown
    panel_->set_form(f);
    QCOMPARE(panel_->form().conditionals, (std::vector<std::string>{"default_unknown", "gone"}));
    QVERIFY(panel_->conditional_choices().contains(QStringLiteral("gone")));
    panel_->set_conditional_checked(QStringLiteral("gone"), false);

    QVERIFY(panel_->add());
    const auto& run = model_->queue().runs.back();
    QCOMPARE(run.id.identifier, std::string("20001"));
    QCOMPARE(run.conditionals.size(), std::size_t{1});
    QCOMPARE(run.conditionals[0].name, std::string("default_unknown"));
    // The next run keeps the choice.
    QCOMPARE(panel_->form().conditionals, std::vector<std::string>{"default_unknown"});

    // A file made while the panel is up appears on refresh; ticks stay.
    std::ofstream(dir_ / "conditionals" / "run_x.toml") << "";
    panel_->refresh_conditionals();
    QVERIFY(panel_->conditional_choices().contains(QStringLiteral("run_x")));
    QCOMPARE(panel_->form().conditionals, std::vector<std::string>{"default_unknown"});
    std::filesystem::remove(dir_ / "conditionals" / "run_x.toml");
  }

  void fromRowFillsTheForm() {
    QVERIFY(!panel_->load_from_selection());
    selection_ = {2};
    QVERIFY(panel_->load_from_selection());
    const FactoryForm f = panel_->form();
    QCOMPARE(f.identifier, std::string("66001"));
    QCOMPARE(f.value, 8.0);
    QCOMPARE(f.duration_s, 10.0);
    QCOMPARE(f.overrides.size(), std::size_t{1});  // "main.cycles" = 1
    QVERIFY(panel_->add());
    QCOMPARE(model_->queue().runs[3].measurement.overrides, model_->queue().runs[2].measurement.overrides);
  }

  void frequencyAndBlocks() {
    // A blank after every unknown in the example queue (two unknowns).
    panel_->set_frequency(AnalysisType::BlankUnknown, 1, false, false);
    QVERIFY(panel_->insert_frequency());
    QCOMPARE(model_->rowCount(), 5);
    QCOMPARE(identifier(2), QStringLiteral("bu"));
    QCOMPARE(identifier(4), QStringLiteral("bu"));
    QCOMPARE(model_->queue().runs[4].measurement.plan, std::string("sim_multicollect"));  // from defaults

    selection_ = {0};
    panel_->set_block(QStringLiteral("blank_pair"), 2);
    QVERIFY(panel_->insert_block());
    QCOMPARE(model_->rowCount(), 9);
    QCOMPARE(inserted_.back(), (std::vector<std::size_t>{1, 2, 3, 4}));
    QVERIFY(model_->runnable());
  }

  void lockedRefusesEverything() {
    panel_->set_form(with_identifier("20001", "1"));
    panel_->set_locked(true);
    QVERIFY(!panel_->add_enabled());
    QVERIFY(!panel_->add());
    QVERIFY(!panel_->insert_frequency());
    QVERIFY(!panel_->insert_block());
    QCOMPARE(model_->rowCount(), 3);
    panel_->set_locked(false);
    QVERIFY(panel_->add_enabled());
    // A locked model (queue running) refuses too.
    model_->set_locked(true);
    QVERIFY(!panel_->add());
    QCOMPARE(model_->rowCount(), 3);
  }

  // While a queue runs, runs go after the rows the executor has reached even
  // when an earlier row is selected.
  void aRunningQueueTakesRunsAfterTheReachedRows() {
    model_->set_live(2, [](std::uint64_t base, const auto&) -> pychron::Result<std::uint64_t> { return base + 1; });
    panel_->set_insert_after_selection(true);
    selection_ = {0};
    panel_->set_form(with_identifier("20001", "1"));
    QVERIFY(panel_->add_enabled());
    QVERIFY(panel_->add());
    QCOMPARE(model_->rowCount(), 4);
    QCOMPARE(inserted_.back(), std::vector<std::size_t>{2});
    QCOMPARE(identifier(2), QStringLiteral("20001"));
    QCOMPARE(model_->version(), 1u);
  }
};

QTEST_MAIN(TestRunFactoryPanel)
#include "test_run_factory_panel.moc"
