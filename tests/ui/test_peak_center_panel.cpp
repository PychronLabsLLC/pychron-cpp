// PeakCenterPanel: the named sets of a peak_center.toml and the override form
// beside them. The file is read, never written, here; every file lives in a
// temp directory.

#include <filesystem>
#include <fstream>
#include <memory>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QtTest/QtTest>

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

  QTemporaryDir dir_;
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
    panel_.reset();
    bridge_.reset();
    sim_.reset();
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
