#include "spectrometer_window.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QShowEvent>
#include <QTableView>
#include <QVBoxLayout>

namespace pychron::ui {

namespace {

constexpr std::array<double, 7> kIntegrationPresets = {0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0};  // seconds
constexpr double kMaxIntegration = 100.0;      // seconds; anything longer in saved settings is ignored
constexpr double kMinScanWidthMinutes = 0.017;  // just over the model's 1 s minimum
constexpr double kMaxScanWidthMinutes = 1440.0;

QString seconds_text(double seconds) { return QStringLiteral("%1 s").arg(QString::number(seconds, 'g', 4)); }

// Saved settings are untrusted: a missing, non-numeric or non-finite value is
// nullopt and the caller keeps its default.
std::optional<double> read_number(const QSettings& settings, const QString& key) {
  if (!settings.contains(key)) {
    return std::nullopt;
  }
  bool ok = false;
  const double value = settings.value(key).toDouble(&ok);
  if (!ok || !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

std::optional<bool> read_flag(const QSettings& settings, const QString& key) {
  const QString text = settings.value(key).toString().trimmed().toLower();
  if (text == QLatin1String("true")) {
    return true;
  }
  if (text == QLatin1String("false")) {
    return false;
  }
  return std::nullopt;
}

}  // namespace

SpectrometerWindow::SpectrometerWindow(SpectrometerBridge& bridge, bool simulation, std::unique_ptr<QSettings> settings,
                                       QWidget* parent)
    : QMainWindow(parent),
      bridge_(bridge),
      settings_(settings ? std::move(settings) : std::make_unique<QSettings>()),
      model_(bridge.detectors()),
      view_(new StripChartView(model_)),
      intensities_(new IntensitiesModel(bridge.detectors(), this)),
      banner_(new QFrame),
      banner_label_(new QLabel) {
  setObjectName(QStringLiteral("SpectrometerWindow"));
  setWindowTitle(simulation ? tr("Spectrometer (Simulation)") : tr("Spectrometer"));
  resize(1100, 650);

  // Centre: the banner (hidden while healthy) above the chart.
  banner_->setObjectName(QStringLiteral("SpectrometerBanner"));
  banner_->setStyleSheet(QStringLiteral("#SpectrometerBanner { background: #f8d7da; } "
                                        "#SpectrometerBanner QLabel { color: #721c24; }"));
  auto* restart = new QPushButton(tr("Restart"));
  auto* banner_row = new QHBoxLayout(banner_);
  banner_label_->setWordWrap(true);
  banner_row->addWidget(banner_label_, 1);
  banner_row->addWidget(restart);
  banner_->hide();
  connect(restart, &QPushButton::clicked, this, [this] { restart_scan(); });

  auto* centre = new QWidget;
  auto* column = new QVBoxLayout(centre);
  column->setContentsMargins(0, 0, 0, 0);
  column->addWidget(banner_);
  column->addWidget(view_, 1);
  setCentralWidget(centre);

  auto* controls = new QDockWidget(tr("Controls"), this);
  controls->setObjectName(QStringLiteral("SpectrometerControlsDock"));
  controls->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);  // not closable
  controls->setWidget(build_controls());
  addDockWidget(Qt::LeftDockWidgetArea, controls);

  auto* table = new QTableView;
  table->setModel(intensities_);
  table->setSelectionMode(QAbstractItemView::NoSelection);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->verticalHeader()->hide();
  table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  table->horizontalHeader()->setStretchLastSection(true);
  auto* intensities = new QDockWidget(tr("Intensities"), this);
  intensities->setObjectName(QStringLiteral("SpectrometerIntensitiesDock"));
  intensities->setWidget(table);
  addDockWidget(Qt::RightDockWidgetArea, intensities);

  confirm_move_ = [this](double delta_amu) {
    const QString question =
        std::isnan(delta_amu)
            ? tr("The current magnet position is unknown. Move the magnet?")
            : tr("This moves %1 by %2 amu. Move the magnet?")
                  .arg(bridge_.reference_detector(), QString::number(delta_amu, 'f', 2));
    return QMessageBox::question(this, tr("Move magnet"), question, QMessageBox::Yes | QMessageBox::No,
                                 QMessageBox::No) == QMessageBox::Yes;
  };

  select_integration(bridge_.default_integration_s());
  load_settings();
  // Connected after the restore: only the user's choice is sent to the scan.
  connect(integration_, &QComboBox::currentIndexChanged, this, [this](int) { bridge_.set_integration(integration_s()); });

  connect(&bridge_, &SpectrometerBridge::readings, this, &SpectrometerWindow::on_readings);
  connect(&bridge_, &SpectrometerBridge::magnetMoved, this, [this] { update_magnet_labels(); });
  connect(&bridge_, &SpectrometerBridge::magnetRead, this, [this] { update_magnet_labels(); });
  connect(&bridge_, &SpectrometerBridge::detectorChanged, this, [this](const spectrometer::DetectorState& e) {
    intensities_->set_isotope(e.detector, QString::fromStdString(e.isotope));
  });
  // Statuses can arrive out of order; the bridge's mirror keeps the newest.
  connect(&bridge_, &SpectrometerBridge::scanStatus, this, [this] {
    update_actual_integration();
    update_banner();
  });
  connect(&bridge_, &SpectrometerBridge::commandFinished, this, &SpectrometerWindow::on_command_finished);
  update_magnet_labels();
  update_actual_integration();
  update_banner();
  sync_y_fields();

  // The repaint cap: the chart is redrawn at most every 50 ms, and only when
  // the model changed.
  connect(&refresh_, &QTimer::timeout, this, [this] {
    if (!dirty_) {
      return;
    }
    dirty_ = false;
    view_->refresh();
    if (model_.autoscale()) {
      sync_y_fields();
    }
  });
  refresh_.start(kRefreshMs);
}

QWidget* SpectrometerWindow::build_controls() {
  auto* panel = new QWidget;
  auto* column = new QVBoxLayout(panel);

  auto* integration_box = new QGroupBox(tr("Integration time"));
  auto* integration_form = new QFormLayout(integration_box);
  integration_ = new QComboBox;
  for (double preset : kIntegrationPresets) {
    integration_->addItem(seconds_text(preset), preset);
  }
  actual_integration_ = new QLabel;
  integration_form->addRow(tr("Requested"), integration_);
  integration_form->addRow(tr("Actual"), actual_integration_);
  column->addWidget(integration_box);

  auto* graph_box = new QGroupBox(tr("Graph"));
  auto* graph_form = new QFormLayout(graph_box);
  scan_width_ = new QDoubleSpinBox;
  scan_width_->setDecimals(3);
  // Typing "15" passes through "1"; a narrower width trims the ring, so the
  // value only applies once the edit is committed.
  scan_width_->setKeyboardTracking(false);
  scan_width_->setRange(kMinScanWidthMinutes, kMaxScanWidthMinutes);
  scan_width_->setValue(model_.scan_width() / 60.0);
  scale_ = new QComboBox;
  scale_->addItem(tr("linear"));
  scale_->addItem(tr("log"));
  autoscale_ = new QCheckBox(tr("Autoscale Y"));
  autoscale_->setChecked(model_.autoscale());
  ymax_ = new QLineEdit;
  ymin_ = new QLineEdit;
  ymax_->setReadOnly(model_.autoscale());
  ymin_->setReadOnly(model_.autoscale());
  auto* clear = new QPushButton(tr("Clear"));
  graph_form->addRow(tr("Scan Width (mins)"), scan_width_);
  graph_form->addRow(tr("Scale"), scale_);
  graph_form->addRow(autoscale_);
  graph_form->addRow(tr("Max"), ymax_);
  graph_form->addRow(tr("Min"), ymin_);
  graph_form->addRow(clear);
  column->addWidget(graph_box);

  connect(scan_width_, &QDoubleSpinBox::valueChanged, this, [this](double minutes) {
    model_.set_scan_width(minutes * 60.0);
    dirty_ = true;
  });
  connect(scale_, &QComboBox::currentIndexChanged, this, [this](int index) {
    model_.set_scale(index == 1 ? YScale::Log : YScale::Linear);
    sync_y_fields();
    dirty_ = true;
  });
  connect(autoscale_, &QCheckBox::toggled, this, [this](bool on) {
    model_.set_autoscale(on);
    ymax_->setReadOnly(on);  // the fields track the live range while autoscale is on
    ymin_->setReadOnly(on);
    sync_y_fields();
    dirty_ = true;
  });
  connect(ymax_, &QLineEdit::editingFinished, this, [this] { edit_manual_y(); });
  connect(ymin_, &QLineEdit::editingFinished, this, [this] { edit_manual_y(); });
  connect(clear, &QPushButton::clicked, this, [this] { clear_chart(); });

  auto* detector_box = new QGroupBox(tr("Detectors"));
  auto* detector_rows = new QVBoxLayout(detector_box);
  const auto& detectors = model_.detectors();
  for (std::size_t i = 0; i < detectors.size(); ++i) {
    auto* swatch = new QLabel;
    swatch->setFixedSize(14, 14);
    swatch->setStyleSheet(QStringLiteral("background: %1;").arg(detectors[i].color.name()));
    auto* shown = new QCheckBox(QString::fromStdString(detectors[i].name));
    shown->setChecked(detectors[i].visible);
    connect(shown, &QCheckBox::toggled, this, [this, i](bool on) {
      model_.set_visible(i, on);
      dirty_ = true;
    });
    shown_.push_back(shown);
    auto* row = new QHBoxLayout;
    row->addWidget(shown);
    row->addWidget(swatch);
    row->addStretch();
    detector_rows->addLayout(row);
  }
  column->addWidget(detector_box);

  auto* magnet_box = new QGroupBox(tr("Magnet"));
  auto* magnet_form = new QFormLayout(magnet_box);
  target_detector_ = new QComboBox;
  for (const auto& detector : detectors) {
    target_detector_->addItem(QString::fromStdString(detector.name));
  }
  target_isotope_ = new QComboBox;
  apply_ = new QPushButton(tr("Apply"));
  position_ = new QLabel;
  mass_ = new QLabel;
  magnet_form->addRow(tr("Detector"), target_detector_);
  magnet_form->addRow(tr("Isotope"), target_isotope_);
  magnet_form->addRow(apply_);
  magnet_form->addRow(tr("Position"), position_);
  magnet_form->addRow(tr("Mass on %1").arg(bridge_.reference_detector()), mass_);
  column->addWidget(magnet_box);
  column->addStretch();

  // Changing a combo moves nothing; only Apply does.
  connect(target_detector_, &QComboBox::currentIndexChanged, this, [this](int) { fill_isotopes(); });
  connect(apply_, &QPushButton::clicked, this, [this] { apply_position(); });
  const int reference = detector_index(bridge_.reference_detector());
  target_detector_->setCurrentIndex(reference >= 0 ? reference : 0);
  fill_isotopes();
  return panel;
}

// The isotopes the field table defines for the selected detector. The current
// choice is kept when the new detector has it too, else the detector's own
// isotope is offered.
void SpectrometerWindow::fill_isotopes() {
  const QString detector = target_detector_->currentText();
  const QString previous = target_isotope_->currentText();
  target_isotope_->clear();
  target_isotope_->addItems(bridge_.isotopes_for(detector));
  int index = target_isotope_->findText(previous);
  if (index < 0) {
    auto current = bridge_.state().isotopes.find(detector.toStdString());
    if (current != bridge_.state().isotopes.end()) {
      index = target_isotope_->findText(QString::fromStdString(current->second));
    }
  }
  target_isotope_->setCurrentIndex(index >= 0 ? index : 0);
}

void SpectrometerWindow::load_settings() {
  QSettings& s = *settings_;
  s.beginGroup(QStringLiteral("spectrometer_window/%1").arg(bridge_.name()));
  if (s.contains(QStringLiteral("geometry"))) {
    restoreGeometry(s.value(QStringLiteral("geometry")).toByteArray());
  }
  if (s.contains(QStringLiteral("dock_state"))) {
    restoreState(s.value(QStringLiteral("dock_state")).toByteArray());
  }
  if (auto width = read_number(s, QStringLiteral("scan_width_s")); width && *width >= 1.0) {
    set_scan_width_minutes(*width / 60.0);
  }
  if (s.value(QStringLiteral("scale")).toString() == QLatin1String("log")) {
    set_scale(YScale::Log);
  }
  const auto ymin = read_number(s, QStringLiteral("ymin"));
  const auto ymax = read_number(s, QStringLiteral("ymax"));
  if (ymin && ymax) {
    set_autoscale(false);  // going manual first: it resets the limits to the live range
    set_manual_y(*ymin, *ymax);
  }
  set_autoscale(read_flag(s, QStringLiteral("autoscale")).value_or(true));
  for (const QString& hidden : s.value(QStringLiteral("hidden_detectors")).toStringList()) {
    set_detector_shown(hidden, false);
  }
  select_target(s.value(QStringLiteral("detector")).toString(), s.value(QStringLiteral("isotope")).toString());
  if (auto integration = read_number(s, QStringLiteral("integration_s"));
      integration && *integration > 0.0 && *integration <= kMaxIntegration) {
    select_integration(*integration);
  }
  if (auto threshold = read_number(s, QStringLiteral("confirm_move_amu")); threshold && *threshold >= 0.0) {
    confirm_move_amu_ = *threshold;
  }
  s.endGroup();
}

void SpectrometerWindow::save_settings() {
  QSettings& s = *settings_;
  s.beginGroup(QStringLiteral("spectrometer_window/%1").arg(bridge_.name()));
  s.setValue(QStringLiteral("geometry"), saveGeometry());
  s.setValue(QStringLiteral("dock_state"), saveState());
  s.setValue(QStringLiteral("scan_width_s"), model_.scan_width());
  s.setValue(QStringLiteral("scale"), model_.scale() == YScale::Log ? QStringLiteral("log") : QStringLiteral("linear"));
  s.setValue(QStringLiteral("autoscale"), model_.autoscale());
  const AxisRange y = model_.y_range(std::chrono::steady_clock::now());
  s.setValue(QStringLiteral("ymin"), y.lo);
  s.setValue(QStringLiteral("ymax"), y.hi);
  QStringList hidden;
  for (const auto& detector : model_.detectors()) {
    if (!detector.visible) {
      hidden.push_back(QString::fromStdString(detector.name));
    }
  }
  s.setValue(QStringLiteral("hidden_detectors"), hidden);
  s.setValue(QStringLiteral("detector"), target_detector_->currentText());
  s.setValue(QStringLiteral("isotope"), target_isotope_->currentText());
  s.setValue(QStringLiteral("integration_s"), integration_s());
  s.setValue(QStringLiteral("confirm_move_amu"), confirm_move_amu_);
  s.endGroup();
  s.sync();
}

void SpectrometerWindow::showEvent(QShowEvent* event) {
  QMainWindow::showEvent(event);
  if (!event->spontaneous()) {  // not a restore from minimised
    bridge_.start_scan(integration_s());
  }
}

void SpectrometerWindow::closeEvent(QCloseEvent* event) {
  bridge_.stop_scan();
  save_settings();
  QMainWindow::closeEvent(event);
}

void SpectrometerWindow::set_confirm_move(std::function<bool(double delta_amu)> confirm) {
  confirm_move_ = std::move(confirm);
}

int SpectrometerWindow::detector_index(const QString& detector) const {
  const std::string name = detector.toStdString();
  const auto& detectors = model_.detectors();
  for (std::size_t i = 0; i < detectors.size(); ++i) {
    if (detectors[i].name == name) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void SpectrometerWindow::set_detector_shown(const QString& detector, bool shown) {
  const int index = detector_index(detector);
  if (index >= 0) {
    shown_[static_cast<std::size_t>(index)]->setChecked(shown);
  }
}

void SpectrometerWindow::select_target(const QString& detector, const QString& isotope) {
  const int index = detector_index(detector);
  if (index < 0) {
    return;
  }
  target_detector_->setCurrentIndex(index);
  const int isotope_index = target_isotope_->findText(isotope);
  if (isotope_index >= 0) {
    target_isotope_->setCurrentIndex(isotope_index);
  }
}

void SpectrometerWindow::apply_position() {
  const QString detector = target_detector_->currentText();
  const QString isotope = target_isotope_->currentText();
  if (move_pending_ || detector.isEmpty() || isotope.isEmpty()) {
    return;
  }
  if (confirm_move_amu_ > 0.0) {
    // Large-move rule: the change of mass on the reference detector. Either
    // side unknown counts as large.
    const auto target = bridge_.mass_on_reference_for(isotope, detector);
    const auto current = bridge_.state().mass_on_reference;
    const double delta = target && current ? *target - *current : std::numeric_limits<double>::quiet_NaN();
    if (!(std::abs(delta) <= confirm_move_amu_) && !confirm_move_(delta)) {
      return;
    }
  }
  move_pending_ = true;
  apply_->setEnabled(false);
  bridge_.position(isotope, detector);
}

double SpectrometerWindow::integration_s() const { return integration_->currentData().toDouble(); }

// A value that is not a preset (a config default, or a test's) gets its own entry.
void SpectrometerWindow::select_integration(double seconds) {
  for (int i = 0; i < integration_->count(); ++i) {
    if (std::abs(integration_->itemData(i).toDouble() - seconds) < 1e-9) {
      integration_->setCurrentIndex(i);
      return;
    }
  }
  integration_->addItem(seconds_text(seconds), seconds);
  integration_->setCurrentIndex(integration_->count() - 1);
}

void SpectrometerWindow::choose_integration(double seconds) {
  if (std::isfinite(seconds) && seconds > 0.0) {
    select_integration(seconds);
  }
}

void SpectrometerWindow::set_scan_width_minutes(double minutes) { scan_width_->setValue(minutes); }

void SpectrometerWindow::set_scale(YScale scale) { scale_->setCurrentIndex(scale == YScale::Log ? 1 : 0); }

void SpectrometerWindow::set_autoscale(bool on) { autoscale_->setChecked(on); }

bool SpectrometerWindow::set_manual_y(double lo, double hi) {
  const bool accepted = model_.set_manual_y(lo, hi);
  sync_y_fields();  // a rejected edit reverts
  dirty_ = true;
  return accepted;
}

void SpectrometerWindow::edit_manual_y() {
  if (model_.autoscale()) {
    return;
  }
  bool lo_ok = false;
  bool hi_ok = false;
  const double lo = ymin_->text().toDouble(&lo_ok);
  const double hi = ymax_->text().toDouble(&hi_ok);
  if (lo_ok && hi_ok) {
    set_manual_y(lo, hi);
  } else {
    sync_y_fields();
  }
}

void SpectrometerWindow::sync_y_fields() {
  const AxisRange y = model_.y_range(std::chrono::steady_clock::now());
  ymin_->setText(QString::number(y.lo, 'g', 6));
  ymax_->setText(QString::number(y.hi, 'g', 6));
}

void SpectrometerWindow::clear_chart() {
  model_.clear();
  dirty_ = true;
}

QString SpectrometerWindow::banner_text() const { return banner_->isHidden() ? QString() : banner_label_->text(); }

void SpectrometerWindow::restart_scan() {
  bridge_.stop_scan();
  bridge_.start_scan(integration_s());
}

QString SpectrometerWindow::position_text() const { return position_->text(); }

QString SpectrometerWindow::mass_text() const { return mass_->text(); }

QString SpectrometerWindow::actual_integration_text() const { return actual_integration_->text(); }

bool SpectrometerWindow::apply_enabled() const { return apply_->isEnabled(); }

// A failed command is shown until the next one succeeds; otherwise the scan's
// own error (a stall, a failed start), which the next start clears.
void SpectrometerWindow::update_banner() {
  const QString text = command_error_.isEmpty() ? QString::fromStdString(bridge_.state().scan.error) : command_error_;
  banner_label_->setText(text);
  banner_->setVisible(!text.isEmpty());
}

void SpectrometerWindow::update_magnet_labels() {
  const auto& state = bridge_.state();
  position_->setText(state.magnet_native ? QString::number(*state.magnet_native, 'f', 5) : QString());
  mass_->setText(state.mass_on_reference ? QString::number(*state.mass_on_reference, 'f', 4) : QString());
}

void SpectrometerWindow::update_actual_integration() {
  const double seconds = std::chrono::duration<double>(bridge_.state().scan.integration).count();
  actual_integration_->setText(seconds > 0.0 ? seconds_text(seconds) : QString());
}

void SpectrometerWindow::on_readings(const std::vector<spectrometer::IntensityReading>& batch) {
  for (const auto& reading : batch) {
    model_.append(reading);
    intensities_->update(reading);
  }
  dirty_ = true;
}

void SpectrometerWindow::on_command_finished(const QString& what, const Result<void>& result) {
  if (what == QLatin1String("position")) {
    move_pending_ = false;
    apply_->setEnabled(true);
  }
  command_error_ = result ? QString() : QStringLiteral("%1: %2").arg(what, QString::fromStdString(result.error().what));
  update_banner();
}

}  // namespace pychron::ui
