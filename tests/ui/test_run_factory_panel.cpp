// RunFactoryPanel against the example lab (its defaults.toml and blocks/) and a
// QueueTableModel holding the example queue. No hardware.

#include <filesystem>
#include <fstream>
#include <memory>

#include <QComboBox>
#include <QLineEdit>
#include <QtTest/QtTest>

#include "experiment_fixture.hpp"
#include "identifier_source.hpp"
#include "queue_table_model.hpp"
#include "run_factory_panel.hpp"

using pychron::experiment::AnalysisType;
using pychron::experiment::FactoryForm;
using pychron::ui::QueueTableModel;
using pychron::ui::RunFactoryPanel;
using pychron::ui::PackageChoice;
using pychron::ui::PackageContents;
namespace lab = pychron::experiment::lab;

namespace {

// Records what the panel asks and answers nothing until the test does, so the
// order of the answers is the test's choice.
struct FakeIdentifierSource : pychron::ui::IdentifierSource {
  using PackagesDone = std::function<void(pychron::Result<std::vector<PackageChoice>>)>;
  using ContentsDone = std::function<void(pychron::Result<PackageContents>)>;
  std::vector<PackagesDone> package_calls;
  std::vector<std::pair<std::string, ContentsDone>> content_calls;
  void packages(QObject*, PackagesDone done) override { package_calls.push_back(std::move(done)); }
  void contents(QObject*, const std::string& package, ContentsDone done) override {
    content_calls.emplace_back(package, std::move(done));
  }
  void notify() { Q_EMIT changed(); }
};

std::vector<PackageChoice> two_packages() { return {{"p2", "NM-294"}, {"p1", "NM-293"}}; }

// Level C holds no identifier.
PackageContents p1_contents() {
  return {{"A", "B", "C"}, {{"66001", "FC-2", "A", 1}, {"66002", "", "A", 3}, {"66010", "bt-1", "B", 2}}};
}

QStringList all_three() {
  return {QStringLiteral("66001  FC-2  (A 1)"), QStringLiteral("66002  (A 3)"), QStringLiteral("66010  bt-1  (B 2)")};
}

}  // namespace

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
  QComboBox* combo(const char* name) const { return panel_->findChild<QComboBox*>(QString::fromLatin1(name)); }
  QLineEdit* edit() const { return panel_->findChild<QLineEdit*>(QStringLiteral("identifier")); }
  // The source set and its packages answered.
  void load(FakeIdentifierSource& fake) {
    panel_->set_identifier_source(&fake);
    fake.package_calls.at(0)(two_packages());
  }
  // NM-293 chosen and its contents answered.
  void choose_p1(FakeIdentifierSource& fake) {
    panel_->choose_package(2);
    fake.content_calls.back().second(p1_contents());
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
    QCOMPARE(panel_->script_choices(),
             (QStringList{QStringLiteral("laser_extract"), QStringLiteral("sim_air"), QStringLiteral("sim_blank"),
                          QStringLiteral("sim_extract")}));
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
  // ---- the identifier select (identifier-select design)

  void noSourceLeavesThePanelAsItWas() {
    QVERIFY(!panel_->selects_visible());
    QVERIFY(panel_->identifier_choices().isEmpty());
    QVERIFY(edit() != nullptr);
  }

  void packagesAreListedAfterNone() {
    FakeIdentifierSource fake;
    panel_->set_identifier_source(&fake);
    QCOMPARE(fake.package_calls.size(), std::size_t{1});
    QVERIFY(!panel_->selects_visible());
    fake.package_calls[0](two_packages());
    QVERIFY(panel_->selects_visible());
    QCOMPARE(panel_->package_choices(),
             (QStringList{QStringLiteral("(none)"), QStringLiteral("NM-294"), QStringLiteral("NM-293")}));
    QVERIFY(panel_->identifier_choices().isEmpty());
    QVERIFY(!combo("level")->isEnabled());
    QVERIFY(fake.content_calls.empty());
  }

  void aPackageListsEveryLevelsIdentifiers() {
    FakeIdentifierSource fake;
    load(fake);
    panel_->choose_package(2);
    QCOMPARE(fake.content_calls.size(), std::size_t{1});
    QCOMPARE(fake.content_calls[0].first, std::string("p1"));
    fake.content_calls[0].second(p1_contents());
    QCOMPARE(panel_->level_choices(), (QStringList{QStringLiteral("(all)"), QStringLiteral("A"), QStringLiteral("B"),
                                                   QStringLiteral("C")}));
    QCOMPARE(panel_->identifier_choices(), all_three());
    QVERIFY(combo("level")->isEnabled());
  }

  void aLevelNarrowsWithoutAskingAgain() {
    FakeIdentifierSource fake;
    load(fake);
    choose_p1(fake);
    panel_->choose_level(1);
    QCOMPARE(panel_->identifier_choices(),
             (QStringList{QStringLiteral("66001  FC-2  (A 1)"), QStringLiteral("66002  (A 3)")}));
    panel_->choose_level(3);
    QVERIFY(panel_->identifier_choices().isEmpty());
    panel_->choose_level(0);
    QCOMPARE(panel_->identifier_choices(), all_three());
    QCOMPARE(fake.content_calls.size(), std::size_t{1});
  }

  void pickingAnItemIsTypingItsIdentifier() {
    FakeIdentifierSource fake;
    load(fake);
    choose_p1(fake);
    QVERIFY(panel_->preview_text().contains(QStringLiteral("identifier")));
    panel_->choose_identifier(2);
    QCOMPARE(panel_->form().identifier, std::string("66010"));
    QCOMPARE(edit()->text(), QStringLiteral("66010"));
    QVERIFY(!panel_->preview_text().contains(QStringLiteral("identifier")));
  }

  void aTypedSpecialStillSwitchesType() {
    FakeIdentifierSource fake;
    load(fake);
    choose_p1(fake);
    panel_->choose_level(1);
    panel_->set_form(with_identifier("20001", "1"));
    QVERIFY(panel_->field_enabled("value"));
    edit()->setText(QString());
    QTest::keyClicks(edit(), QStringLiteral("a"));
    QCOMPARE(panel_->form().identifier, std::string("a"));
    QVERIFY(!panel_->field_enabled("value"));  // air: its type's rules, as when there is no select
    QCOMPARE(combo("package")->currentIndex(), 2);
    QCOMPARE(combo("level")->currentIndex(), 1);
  }

  void setFormLeavesTheSelects() {
    FakeIdentifierSource fake;
    load(fake);
    choose_p1(fake);
    panel_->choose_level(1);
    const QStringList packages = panel_->package_choices();
    const QStringList identifiers = panel_->identifier_choices();
    panel_->set_form(with_identifier("20001", "1"));
    QCOMPARE(edit()->text(), QStringLiteral("20001"));
    QCOMPARE(panel_->package_choices(), packages);
    QCOMPARE(panel_->identifier_choices(), identifiers);
    QCOMPARE(combo("package")->currentIndex(), 2);
    QCOMPARE(combo("level")->currentIndex(), 1);
  }

  void choosingAnotherPackageResetsLevel() {
    FakeIdentifierSource fake;
    load(fake);
    choose_p1(fake);
    panel_->choose_level(2);
    panel_->choose_package(1);
    QCOMPARE(fake.content_calls.back().first, std::string("p2"));
    QCOMPARE(combo("level")->currentIndex(), 0);
    QCOMPARE(panel_->level_choices(), QStringList{QStringLiteral("(all)")});
    QVERIFY(panel_->identifier_choices().isEmpty());
    panel_->choose_package(0);
    QVERIFY(!combo("level")->isEnabled());
    QVERIFY(panel_->identifier_choices().isEmpty());
    QCOMPARE(fake.content_calls.size(), std::size_t{2});
  }

  void packagesOfOneNameAreToldApartById() {
    FakeIdentifierSource fake;
    panel_->set_identifier_source(&fake);
    fake.package_calls.at(0)(std::vector<PackageChoice>{{"x", "Dup"}, {"y", "Dup"}});
    panel_->choose_package(2);
    QCOMPARE(fake.content_calls.back().first, std::string("y"));
  }

  void anEmptyPackageIsNotAnError() {
    FakeIdentifierSource fake;
    load(fake);
    panel_->choose_package(2);
    fake.content_calls.back().second(PackageContents{});
    QCOMPARE(panel_->level_choices(), QStringList{QStringLiteral("(all)")});
    QVERIFY(panel_->identifier_choices().isEmpty());
    QVERIFY(panel_->identifier_tooltip().isEmpty());
  }

  void lockedDisablesTheSelects() {
    FakeIdentifierSource fake;
    load(fake);
    panel_->set_locked(true);
    QVERIFY(!combo("package")->isEnabled());
    QVERIFY(!combo("level")->isEnabled());
    QVERIFY(!combo("identifier_select")->isEnabled());
    panel_->set_locked(false);
    QVERIFY(combo("package")->isEnabled());
    QVERIFY(combo("identifier_select")->isEnabled());
    QVERIFY(!combo("level")->isEnabled());
  }
  void aLateAnswerForAnotherPackageIsDropped() {
    FakeIdentifierSource fake;
    load(fake);
    panel_->choose_package(2);
    panel_->choose_package(1);
    QCOMPARE(fake.content_calls.size(), std::size_t{2});
    fake.content_calls[1].second(PackageContents{{"A"}, {{"70001", "", "A", 1}}});
    fake.content_calls[0].second(p1_contents());
    QCOMPARE(panel_->identifier_choices(), QStringList{QStringLiteral("70001  (A 1)")});
    QCOMPARE(panel_->level_choices(), (QStringList{QStringLiteral("(all)"), QStringLiteral("A")}));
  }

  void aCatalogChangeReloadsAndKeepsTheChoice() {
    FakeIdentifierSource fake;
    load(fake);
    choose_p1(fake);
    panel_->choose_level(2);
    panel_->choose_identifier(0);
    QCOMPARE(edit()->text(), QStringLiteral("66010"));
    fake.notify();
    QCOMPARE(fake.package_calls.size(), std::size_t{2});
    fake.package_calls[1](two_packages());
    QCOMPARE(fake.content_calls.size(), std::size_t{2});
    QCOMPARE(fake.content_calls[1].first, std::string("p1"));
    PackageContents more = p1_contents();
    more.choices.push_back({"66011", "", "B", 4});
    fake.content_calls[1].second(more);
    QCOMPARE(combo("package")->currentIndex(), 2);
    QCOMPARE(combo("level")->currentText(), QStringLiteral("B"));
    QCOMPARE(panel_->identifier_choices(),
             (QStringList{QStringLiteral("66010  bt-1  (B 2)"), QStringLiteral("66011  (B 4)")}));
    QCOMPARE(edit()->text(), QStringLiteral("66010"));
  }

  void aCatalogChangeDropsAChoiceThatIsGone() {
    FakeIdentifierSource fake;
    load(fake);
    choose_p1(fake);
    panel_->choose_level(2);
    // The level went away: back to all of the package.
    fake.notify();
    fake.package_calls.back()(two_packages());
    fake.content_calls.back().second(PackageContents{{"A"}, {{"66001", "FC-2", "A", 1}}});
    QCOMPARE(combo("package")->currentIndex(), 2);
    QCOMPARE(combo("level")->currentIndex(), 0);
    QCOMPARE(panel_->identifier_choices(), QStringList{QStringLiteral("66001  FC-2  (A 1)")});
    // The package went away: nothing chosen, and nothing asked about it.
    const std::size_t asked = fake.content_calls.size();
    fake.notify();
    fake.package_calls.back()(std::vector<PackageChoice>{{"p2", "NM-294"}});
    QCOMPARE(combo("package")->currentIndex(), 0);
    QVERIFY(!combo("level")->isEnabled());
    QVERIFY(panel_->identifier_choices().isEmpty());
    QCOMPARE(fake.content_calls.size(), asked);
  }

  void aChangeWhileContentsArePendingWins() {
    FakeIdentifierSource fake;
    load(fake);
    panel_->choose_package(2);
    fake.notify();
    fake.package_calls.back()(two_packages());
    QCOMPARE(fake.content_calls.size(), std::size_t{2});
    fake.content_calls[1].second(PackageContents{{"A"}, {{"70001", "", "A", 1}}});
    fake.content_calls[0].second(p1_contents());
    QCOMPARE(panel_->identifier_choices(), QStringList{QStringLiteral("70001  (A 1)")});
  }

  void aFirstPackagesFailureKeepsTheRowsHidden() {
    FakeIdentifierSource fake;
    panel_->set_identifier_source(&fake);
    fake.package_calls.at(0)(pychron::fail(pychron::ErrorKind::Io, "store gone"));
    QVERIFY(!panel_->selects_visible());
  }

  void aLaterPackagesFailureKeepsTheList() {
    FakeIdentifierSource fake;
    load(fake);
    const QStringList before = panel_->package_choices();
    fake.notify();
    fake.package_calls.back()(pychron::fail(pychron::ErrorKind::Io, "store gone"));
    QCOMPARE(panel_->package_choices(), before);
    QVERIFY(panel_->selects_visible());
    QVERIFY(panel_->package_tooltip().contains(QStringLiteral("store gone")));
    fake.notify();
    fake.package_calls.back()(two_packages());
    QVERIFY(panel_->package_tooltip().isEmpty());
  }

  void aContentsFailureEmptiesTheListAndSaysWhy() {
    FakeIdentifierSource fake;
    load(fake);
    choose_p1(fake);
    panel_->choose_package(2);
    fake.content_calls.back().second(pychron::fail(pychron::ErrorKind::Io, "no such level"));
    QCOMPARE(panel_->level_choices(), QStringList{QStringLiteral("(all)")});
    QVERIFY(panel_->identifier_choices().isEmpty());
    QVERIFY(panel_->identifier_tooltip().contains(QStringLiteral("no such level")));
    choose_p1(fake);
    QVERIFY(panel_->identifier_tooltip().isEmpty());
    QCOMPARE(panel_->identifier_choices(), all_three());
  }

  void anAnswerWhileLockedFillsButStaysDisabled() {
    FakeIdentifierSource fake;
    load(fake);
    panel_->choose_package(2);
    panel_->set_locked(true);
    fake.content_calls.back().second(p1_contents());
    QCOMPARE(panel_->identifier_choices(), all_three());
    for (const char* name : {"package", "level", "identifier_select"}) QVERIFY(!combo(name)->isEnabled());
    panel_->set_locked(false);
    for (const char* name : {"package", "level", "identifier_select"}) QVERIFY(combo(name)->isEnabled());
  }

  void aSourceThatGoesAwayHidesTheSelects() {
    auto* fake = new FakeIdentifierSource;
    load(*fake);
    choose_p1(*fake);
    panel_->choose_identifier(0);
    delete fake;
    QVERIFY(!panel_->selects_visible());
    QVERIFY(panel_->identifier_choices().isEmpty());
    QCOMPARE(edit()->text(), QStringLiteral("66001"));
    panel_->set_form(panel_->form());
    QCOMPARE(panel_->form().identifier, std::string("66001"));
  }

  void aSourceCanBeReplacedOrCleared() {
    FakeIdentifierSource a;
    FakeIdentifierSource b;
    load(a);
    choose_p1(a);
    panel_->choose_package(1);  // left unanswered
    panel_->set_identifier_source(nullptr);
    QVERIFY(!panel_->selects_visible());
    QCOMPARE(panel_->package_choices(), QStringList{QStringLiteral("(none)")});
    QVERIFY(panel_->identifier_choices().isEmpty());
    panel_->set_identifier_source(&b);
    QCOMPARE(b.package_calls.size(), std::size_t{1});
    a.notify();
    QCOMPARE(a.package_calls.size(), std::size_t{1});
    QCOMPARE(b.package_calls.size(), std::size_t{1});
    a.content_calls.back().second(p1_contents());
    QVERIFY(panel_->identifier_choices().isEmpty());
    QVERIFY(!panel_->selects_visible());
  }
};

QTEST_MAIN(TestRunFactoryPanel)
#include "test_run_factory_panel.moc"
