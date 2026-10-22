// PeakCenterPanel: the named sets of a peak_center.toml and the override form
// beside them. The file is read, never written, here; every file lives in a
// temp directory.

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "pychron/core/virtual_clock.hpp"

#include "peak_center_panel.hpp"
#include "spectrometer_bridge.hpp"
#include "spectrometer_fixture.hpp"

using namespace pychron;
using pychron::ui::PeakCenterPanel;
using pychron::ui::SpectrometerBridge;

namespace {

template <class T>
T* child(QWidget& parent, const char* name) {
  auto* w = parent.findChild<T*>(QString::fromLatin1(name));
  if (w == nullptr) qFatal("no object named %s", name);
  return w;
}

QStringList items(const QComboBox* combo) {
  QStringList out;
  for (int i = 0; i < combo->count(); ++i) out << combo->itemText(i);
  return out;
}

}  // namespace

class TestPeakCenterPanel : public QObject {
  Q_OBJECT

 private:
  std::filesystem::path write(const QString& text) {
    const auto path = std::filesystem::path(dir_.filePath(QStringLiteral("pc-%1.toml").arg(file_++)).toStdString());
    std::ofstream(path, std::ios::binary) << text.toStdString();
    return path;
  }

  // The sim on a VirtualClock at full speed with a copy of the example tables
  // (Apply writes a table), as the bridge suite's peak center cases do.
  void virtual_sim() {
    panel_.reset();
    bridge_.reset();
    sim_.reset();
    clock_ = std::make_unique<VirtualClock>();
    tables_ = std::make_unique<QTemporaryDir>();
    QVERIFY(tables_->isValid());
    const std::filesystem::path root = tables_->path().toStdString();
    std::filesystem::copy(std::filesystem::path(PYCHRON_EXAMPLE_CONFIGS_DIR) / "tables", root / "tables",
                          std::filesystem::copy_options::recursive);
    sim_ = ui::test::make_sim_spectrometer([&root](spectrometer::cfg::SpectrometerData& data) { data.root = root; },
                                           clock_.get());
    bridge_ = std::make_unique<SpectrometerBridge>(*sim_->spec, *sim_->scan, sim_->bus);
    panel_ = std::make_unique<PeakCenterPanel>(*bridge_);
    asked_.clear();
    panel_->set_ask([this](const QString& q) {
      asked_.push_back(q);
      return answer_;
    });
  }
  static constexpr const char* kAr40 = "[p]\nisotope = \"Ar40\"\nwindow = 0.06\nstep = 0.002\nintegration_s = 0.1\n";
  // Runs set `p` of the file to its end; true when it gave a result.
  bool run_and_wait() {
    QSignalSpy finished(bridge_.get(), &SpectrometerBridge::peakCenterFinished);
    QTest::mouseClick(child<QPushButton>(*panel_, "peakCenterStart"), Qt::LeftButton);
    return finished.wait(10000) || finished.count() == 1;
  }
  QString text(const char* name) { return child<QLabel>(*panel_, name)->text(); }
  bool enabled(const char* name) { return child<QPushButton>(*panel_, name)->isEnabled(); }

  QTemporaryDir dir_;
  std::unique_ptr<QTemporaryDir> tables_;
  std::unique_ptr<VirtualClock> clock_;  // outlives sim_
  QStringList asked_;
  bool answer_ = true;
  int file_ = 0;
  std::unique_ptr<ui::test::SimSpectrometer> sim_;
  std::unique_ptr<SpectrometerBridge> bridge_;
  std::unique_ptr<PeakCenterPanel> panel_;

 private slots:
  void init() {
    sim_ = ui::test::make_sim_spectrometer();
    bridge_ = std::make_unique<SpectrometerBridge>(*sim_->spec, *sim_->scan, sim_->bus);
    panel_ = std::make_unique<PeakCenterPanel>(*bridge_);
  }
  void cleanup() {
    if (clock_) clock_->set_speed(std::numeric_limits<double>::infinity());
    panel_.reset();
    bridge_.reset();
    sim_.reset();
    clock_.reset();
    tables_.reset();
  }

  void setLoadsForm() {
    panel_->set_file(write(QStringLiteral("[default]\n[tuned]\nwindow = 0.02\nn_tries = 3\n")));
    panel_->select_set(QStringLiteral("tuned"));
    QCOMPARE(panel_->set_name(), QStringLiteral("tuned"));
    QCOMPARE(child<QDoubleSpinBox>(*panel_, "pc_window")->value(), 0.02);
    QCOMPARE(child<QSpinBox>(*panel_, "pc_n_tries")->value(), 3);
    QVERIFY(!panel_->edited());
  }

  void editMarksAndRevert() {
    panel_->set_file(write(QStringLiteral("[tuned]\nwindow = 0.02\nn_tries = 3\n")));
    panel_->select_set(QStringLiteral("tuned"));
    auto* label = child<QLabel>(*panel_, "pc_window_label");
    QVERIFY(!label->font().bold());
    child<QDoubleSpinBox>(*panel_, "pc_window")->setValue(0.03);
    QVERIFY(panel_->edited());
    QVERIFY(label->font().bold());
    QVERIFY(!child<QLabel>(*panel_, "pc_n_tries_label")->font().bold());
    panel_->revert();
    QVERIFY(!panel_->edited());
    QVERIFY(!label->font().bold());
    QCOMPARE(child<QDoubleSpinBox>(*panel_, "pc_window")->value(), 0.02);
  }

  void invalidFormDisablesStartAndSave() {
    panel_->set_file(write(QStringLiteral("[tuned]\nwindow = 0.02\n")));
    panel_->select_set(QStringLiteral("tuned"));
    QVERIFY(child<QPushButton>(*panel_, "peakCenterStart")->isEnabled());
    QVERIFY(child<QPushButton>(*panel_, "peakCenterSave")->isEnabled());
    child<QDoubleSpinBox>(*panel_, "pc_step")->setValue(0.05);
    QVERIFY(!child<QPushButton>(*panel_, "peakCenterStart")->isEnabled());
    QVERIFY(!child<QPushButton>(*panel_, "peakCenterSave")->isEnabled());
    QVERIFY(child<QLabel>(*panel_, "peakCenterReason")->text().contains(
        QStringLiteral("step must be positive and smaller than window")));
    child<QDoubleSpinBox>(*panel_, "pc_step")->setValue(0.001);
    QVERIFY(child<QPushButton>(*panel_, "peakCenterStart")->isEnabled());
    QVERIFY(child<QLabel>(*panel_, "peakCenterReason")->text().isEmpty());
  }

  void missingFileOffersBuiltInDefault() {
    panel_->set_file(std::filesystem::path(dir_.filePath(QStringLiteral("absent.toml")).toStdString()));
    QCOMPARE(items(child<QComboBox>(*panel_, "peakCenterSet")), QStringList{QStringLiteral("default")});
    QCOMPARE(panel_->config(), jobs::PeakCenterConfig{});
    QVERIFY(child<QLabel>(*panel_, "peakCenterBanner")->text().isEmpty());
  }

  void fileWithoutDefaultOffersIt() {
    panel_->set_file(write(QStringLiteral("[tuned]\nwindow = 0.02\n")));
    QCOMPARE(items(child<QComboBox>(*panel_, "peakCenterSet")),
             (QStringList{QStringLiteral("default"), QStringLiteral("tuned")}));
  }

  void unparseableFileDisablesSave() {
    panel_->set_file(write(QStringLiteral("[x]\nwindow = -1\n")));
    QCOMPARE(items(child<QComboBox>(*panel_, "peakCenterSet")), QStringList{QStringLiteral("default")});
    QVERIFY(child<QLabel>(*panel_, "peakCenterBanner")->text().contains(QStringLiteral("window must be positive")));
    QVERIFY(!child<QPushButton>(*panel_, "peakCenterSave")->isEnabled());
    QVERIFY(child<QPushButton>(*panel_, "peakCenterStart")->isEnabled());
  }

  void isotopesFollowDetector() {
    auto* detector = child<QComboBox>(*panel_, "pc_detector");
    const int h1 = detector->findText(QStringLiteral("H1"));
    QVERIFY(h1 >= 0);
    detector->setCurrentIndex(h1);
    QCOMPARE(items(child<QComboBox>(*panel_, "pc_isotope")), bridge_->isotopes_for(QStringLiteral("H1")));
    QCOMPARE(QString::fromStdString(panel_->config().detector), QStringLiteral("H1"));
  }

  void exactValuesSurviveUntilEdited() {
    const QString text = QStringLiteral("[p]\nwindow = 0.02\nstep = 1.234567891e-4\ndac_offset = 2.5e-7\n");
    const auto path = write(text);
    panel_->set_file(path);
    panel_->select_set(QStringLiteral("p"));
    std::ifstream in(path);
    const std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto parsed = jobs::parse_peak_center_configs(body);
    QVERIFY(parsed.has_value());
    QCOMPARE(panel_->config(), parsed->at("p"));
    QVERIFY(!panel_->edited());
    child<QDoubleSpinBox>(*panel_, "pc_window")->setValue(0.03);
    QCOMPARE(panel_->config().window, 0.03);
    QCOMPARE(panel_->config().dac_offset, 2.5e-7);
    QCOMPARE(panel_->config().step, parsed->at("p").step);
  }

  void unknownAdditionalDetectorsKept() {
    panel_->set_file(write(QStringLiteral("[p]\nadditional_detectors = [\"H1\", \"ZZ9\"]\n")));
    panel_->select_set(QStringLiteral("p"));
    const std::vector<std::string> want{"H1", "ZZ9"};
    QCOMPARE(panel_->config().additional_detectors, want);
    QVERIFY(!panel_->edited());
  }

  void runShowsResult() {
    virtual_sim();
    panel_->set_file(write(QString::fromLatin1(kAr40)));
    panel_->select_set(QStringLiteral("p"));
    QSignalSpy started(panel_.get(), &PeakCenterPanel::startRequested);
    QVERIFY(run_and_wait());
    QCOMPARE(started.count(), 1);
    QVERIFY(!bridge_->peak_center_running());
    QVERIFY2(text("peakCenterResultStatus").size() > 0, "status line");
    const double initial = text("peakCenterResultInitial").toDouble();
    const double fitted = text("peakCenterResultCenter").toDouble();
    QVERIFY(!text("peakCenterResultCenter").isEmpty());
    QCOMPARE(text("peakCenterResultShift"), QString::number(fitted - initial, 'f', 6));
    QVERIFY(!text("peakCenterResultTableValue").isEmpty());
    QVERIFY(!enabled("peakCenterCancel"));
  }

  void startDisabledWhileRunning() {
    virtual_sim();
    panel_->set_file(write(QString::fromLatin1(kAr40)));
    panel_->select_set(QStringLiteral("p"));
    QSignalSpy finished(bridge_.get(), &SpectrometerBridge::peakCenterFinished);
    clock_->set_speed(0);  // the run starts and waits in the clock
    QTest::mouseClick(child<QPushButton>(*panel_, "peakCenterStart"), Qt::LeftButton);
    QVERIFY(bridge_->peak_center_running());
    QVERIFY(!enabled("peakCenterStart"));
    QVERIFY(enabled("peakCenterCancel"));
    QTest::mouseClick(child<QPushButton>(*panel_, "peakCenterCancel"), Qt::LeftButton);
    clock_->set_speed(std::numeric_limits<double>::infinity());
    QVERIFY(finished.wait(10000) || finished.count() == 1);
    QVERIFY(enabled("peakCenterStart"));
    QVERIFY(!text("peakCenterResultStatus").isEmpty());  // the error text
    QVERIFY(text("peakCenterResultCenter").isEmpty());
  }

  void queueRunningDisablesStartAndApply() {
    virtual_sim();
    bool queue = true;
    panel_->set_file(write(QString::fromLatin1(kAr40)));
    panel_->select_set(QStringLiteral("p"));
    panel_->set_queue_running([&queue] { return queue; });
    QVERIFY(!enabled("peakCenterStart"));
    QCOMPARE(child<QPushButton>(*panel_, "peakCenterStart")->toolTip(), QStringLiteral("a queue is running"));
    QSignalSpy started(panel_.get(), &PeakCenterPanel::startRequested);
    QTest::mouseClick(child<QPushButton>(*panel_, "peakCenterStart"), Qt::LeftButton);
    QCOMPARE(started.count(), 0);
    queue = false;
    QTest::mouseClick(child<QPushButton>(*panel_, "peakCenterStart"), Qt::LeftButton);  // disabled: no effect
    panel_->set_queue_running([&queue] { return queue; });
    QVERIFY(enabled("peakCenterStart"));
    QVERIFY(run_and_wait());
    QVERIFY(enabled("peakCenterApply"));
    queue = true;
    panel_->set_queue_running([&queue] { return queue; });
    QVERIFY(!enabled("peakCenterApply"));
    panel_->set_queue_running({});
  }

  void applyEnabledOnlyAfterOk() {
    virtual_sim();
    panel_->set_file(write(QString::fromLatin1(kAr40)));
    panel_->select_set(QStringLiteral("p"));
    QVERIFY(!enabled("peakCenterApply"));
    QVERIFY(run_and_wait());
    QVERIFY(enabled("peakCenterApply"));
    QSignalSpy applied(bridge_.get(), &SpectrometerBridge::peakCenterApplied);
    QTest::mouseClick(child<QPushButton>(*panel_, "peakCenterApply"), Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(applied.count(), 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!enabled("peakCenterApply"), 10000);
    QTest::qWait(100);  // and it stays so: the table change Apply made does not enable it
    QVERIFY(!enabled("peakCenterApply"));
  }

  void applyDisabledWithoutIsotope() {
    virtual_sim();
    panel_->set_file(write(QString::fromLatin1(kAr40)));
    panel_->select_set(QStringLiteral("p"));
    QVERIFY(run_and_wait());
    const double center = text("peakCenterResultCenter").toDouble();
    panel_->set_file(write(QStringLiteral("[q]\nisotope = \"\"\ncenter = %1\nwindow = 0.06\nstep = 0.002\nintegration_s = 0.1\n")
                               .arg(center, 0, 'f', 6)));
    panel_->select_set(QStringLiteral("q"));
    QCOMPARE(panel_->config().isotope, std::string());
    QVERIFY(run_and_wait());
    QVERIFY(text("peakCenterResultCenter").size() > 0);
    QVERIFY(!enabled("peakCenterApply"));
  }

  void applyDisabledAfterTableChanged() {
    virtual_sim();
    panel_->set_file(write(QString::fromLatin1(kAr40)));
    panel_->select_set(QStringLiteral("p"));
    QVERIFY(run_and_wait());
    QVERIFY(enabled("peakCenterApply"));
    QSignalSpy changed(bridge_.get(), &SpectrometerBridge::fieldTableChanged);
    bridge_->replace_table(bridge_->active_table(), bridge_->table_copy(bridge_->active_table()), QString());
    QTRY_VERIFY_WITH_TIMEOUT(changed.count() >= 1, 10000);
    QVERIFY(!enabled("peakCenterApply"));
  }

  void applyAsksAndWrites() {
    virtual_sim();
    panel_->set_file(write(QString::fromLatin1(kAr40)));
    panel_->select_set(QStringLiteral("p"));
    QVERIFY(run_and_wait());
    const double value = text("peakCenterResultTableValue").toDouble();
    answer_ = false;
    QTest::mouseClick(child<QPushButton>(*panel_, "peakCenterApply"), Qt::LeftButton);
    QCOMPARE(asked_.size(), 1);
    const QString want = QStringLiteral("Write %1 for Ar40 on %2 to %3? Propagate: yes.")
                             .arg(QString::number(value, 'f', 6), QString::fromStdString(panel_->config().detector.empty()
                                                                                              ? bridge_->reference_detector().toStdString()
                                                                                              : panel_->config().detector),
                                  bridge_->active_table());
    QCOMPARE(asked_[0], want);
    QVERIFY(enabled("peakCenterApply"));  // refused: nothing written
    answer_ = true;
    QSignalSpy applied(bridge_.get(), &SpectrometerBridge::peakCenterApplied);
    QTest::mouseClick(child<QPushButton>(*panel_, "peakCenterApply"), Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(applied.count(), 1, 10000);
    QVERIFY(applied.at(0).at(0).value<Result<void>>().has_value());
    QVERIFY(text("peakCenterResultStatus").contains(bridge_->active_table()));
  }

  void saveAsksBeforeOverwrite() {
    const QString original = QStringLiteral("# keep\n[default]\n[tuned]\nwindow = 0.02\n");
    const auto path = write(original);
    asked_.clear();
    panel_->set_file(path);
    panel_->select_set(QStringLiteral("tuned"));
    panel_->set_ask([this](const QString& q) {
      asked_.push_back(q);
      return answer_;
    });
    panel_->set_ask_name([](QString suggestion) { return std::optional<QString>(suggestion); });
    child<QDoubleSpinBox>(*panel_, "pc_window")->setValue(0.03);
    QVERIFY(panel_->edited());
    answer_ = false;
    QTest::mouseClick(child<QPushButton>(*panel_, "peakCenterSave"), Qt::LeftButton);
    QCOMPARE(asked_.size(), 1);
    QTest::qWait(200);
    const auto read = [&path] {
      std::ifstream in(path, std::ios::binary);
      return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    QCOMPARE(QString::fromStdString(read()), original);  // refused: unchanged
    answer_ = true;
    QSignalSpy saved(bridge_.get(), &SpectrometerBridge::peakCenterSetSaved);
    QTest::mouseClick(child<QPushButton>(*panel_, "peakCenterSave"), Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!panel_->edited(), 10000);
    QCOMPARE(panel_->set_name(), QStringLiteral("tuned"));
    QCOMPARE(child<QDoubleSpinBox>(*panel_, "pc_window")->value(), 0.03);
    QVERIFY(QString::fromStdString(read()).contains(QStringLiteral("# keep")));
  }

  void saveNewNameAndBadName() {
    const auto path = write(QStringLiteral("[default]\n"));
    panel_->set_file(path);
    panel_->set_ask([](const QString&) { return true; });
    QString name = QStringLiteral("bad name");
    panel_->set_ask_name([&name](const QString&) { return std::optional<QString>(name); });
    QSignalSpy saved(bridge_.get(), &SpectrometerBridge::peakCenterSetSaved);
    QTest::mouseClick(child<QPushButton>(*panel_, "peakCenterSave"), Qt::LeftButton);
    QCOMPARE(saved.count(), 0);
    QVERIFY(!text("peakCenterReason").isEmpty());
    name = QStringLiteral("fresh");
    QTest::mouseClick(child<QPushButton>(*panel_, "peakCenterSave"), Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(panel_->set_name(), QStringLiteral("fresh"), 10000);
    QVERIFY(!panel_->edited());
  }

  void configRoundTrip() {
    const QString text = QStringLiteral(
        "[a]\ndetector = \"H1\"\nisotope = \"Ar40\"\nadditional_detectors = [\"AX\"]\ncenter = 5.5\n"
        "window = 0.03\nstep = 0.001\npercent = 70\nmin_peak_height = 2.5\ntest_peak_flat = false\n"
        "n_tries = 4\ndirection = \"decrease\"\nintegration_s = 0.5\nsettle_s = 0.25\n"
        "baseline_timeout_s = 7\ndac_offset = 0.002\npeak_shift_threshold = 0.01\n"
        "update_table = false\npropagate = false\n");
    const auto path = write(text);
    panel_->set_file(path);
    panel_->select_set(QStringLiteral("a"));
    std::ifstream in(path);
    const std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto parsed = jobs::parse_peak_center_configs(body);
    QVERIFY(parsed.has_value());
    QCOMPARE(panel_->config(), parsed->at("a"));
    QVERIFY(!panel_->edited());
  }
};

QTEST_MAIN(TestPeakCenterPanel)
#include "test_peak_center_panel.moc"
