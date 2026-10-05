#include "laser_window.hpp"

#include <cmath>
#include <numbers>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include "camera_view.hpp"
#include "pychron/laser/pattern.hpp"
#include "theme.hpp"
#include "tray_view.hpp"

namespace pychron::ui {

namespace {

QString mm(double value, int places = 3) { return QString::number(value, 'f', places); }

QPushButton* button(const QString& text, const char* name, QWidget* parent) {
  auto* b = new QPushButton(text, parent);
  b->setObjectName(QString::fromLatin1(name));
  b->setAutoDefault(false);
  return b;
}

QTableWidget* table(const QStringList& headers, const char* name, QWidget* parent) {
  auto* t = new QTableWidget(0, static_cast<int>(headers.size()), parent);
  t->setObjectName(QString::fromLatin1(name));
  t->setHorizontalHeaderLabels(headers);
  t->setEditTriggers(QAbstractItemView::NoEditTriggers);
  t->setSelectionBehavior(QAbstractItemView::SelectRows);
  t->setSelectionMode(QAbstractItemView::SingleSelection);
  t->verticalHeader()->hide();
  t->horizontalHeader()->setStretchLastSection(true);
  return t;
}

QString outcome_text(const laser::AutocenterOutcome& o) {
  using R = laser::AutocenterOutcome::Result;
  const QString hole = QString::fromStdString(o.hole);
  switch (o.result) {
    case R::None: return {};
    case R::Converged:
      return LaserWindow::tr("hole %1 centred: moved %2, %3 mm (%4 looks)")
          .arg(hole, mm(o.moved_mm.x), mm(o.moved_mm.y))
          .arg(o.iterations);
    case R::Failed:
      return LaserWindow::tr("hole %1 not centred: %2").arg(hole, QString::fromStdString(std::string(to_string(o.reason))));
    case R::Stopped: return LaserWindow::tr("centring hole %1 was stopped").arg(hole);
  }
  return {};
}

}  // namespace

LaserWindow::LaserWindow(LaserBridge& bridge, bool simulation, QWidget* parent)
    : QMainWindow(parent), bridge_(bridge), simulation_(simulation) {
  setObjectName(QStringLiteral("laser_window"));
  setWindowTitle(tr("Laser %1%2").arg(bridge_.device(), simulation_ ? tr(" (Simulation)") : QString()));
  build();
  connect(&bridge_, &LaserBridge::snapshot, this, &LaserWindow::on_snapshot);
  connect(&bridge_, &LaserBridge::commandFinished, this, &LaserWindow::on_finished);
  connect(&bridge_, &LaserBridge::driverChanged, this, [this](bool) { refresh_enabled(); });
  connect(&bridge_, &LaserBridge::calibrationChanged, this, &LaserWindow::refresh_tray);
  connect(&bridge_, &LaserBridge::view, camera_, &CameraView::set_view);
  connect(&bridge_, &LaserBridge::viewFailed, camera_, &CameraView::set_failed);
  if (!bridge_.has_camera()) camera_->clear(tr("%1 has no camera").arg(bridge_.device()));
  refresh_patterns();
  on_snapshot(bridge_.state());
}

void LaserWindow::build() {
  auto* central = new QWidget(this);
  auto* outer = new QVBoxLayout(central);

  // The top bar: the tray, what is going on, and the stop.
  auto* top = new QHBoxLayout;
  top->addWidget(new QLabel(tr("Tray"), central));
  tray_choice_ = new QComboBox(central);
  tray_choice_->setObjectName(QStringLiteral("tray_choice"));
  tray_choice_->addItem(tr("(none)"), QString());
  for (const QString& name : bridge_.trays()) tray_choice_->addItem(name, name);
  top->addWidget(tray_choice_);
  banner_ = new QLabel(central);
  banner_->setObjectName(QStringLiteral("banner"));
  style::make_banner(banner_);
  banner_->hide();
  top->addWidget(banner_, 1);
  top->addStretch(0);
  reset_ = button(tr("Reset"), "reset_stop", central);
  reset_->setToolTip(tr("Allow the laser and the stage to be driven again"));
  reset_->hide();
  top->addWidget(reset_);
  estop_ = button(tr("EMERGENCY STOP"), "estop", central);
  estop_->setToolTip(tr("Beam off, output 0, laser disabled, stage stopped, queue aborted"));
  estop_->setMinimumHeight(40);
  // The one control that is never disabled, and never looks it.
  estop_->setStyleSheet(
      QStringLiteral("QPushButton#estop { background: %1; color: %3; border: 1px solid %2; font-weight: 700;"
                     " padding: 6px 18px; }"
                     "QPushButton#estop:hover { background: %2; }"
                     "QPushButton#estop:pressed { background: %2; }")
          .arg(theme().error_text.name(), theme().error_text.darker(125).name(), theme().base.name()));
  top->addWidget(estop_);
  outer->addLayout(top);

  auto* split = new QSplitter(Qt::Horizontal, central);
  tray_ = new TrayView(split);
  split->addWidget(tray_);

  auto* right = new QWidget(split);
  auto* column = new QVBoxLayout(right);
  column->setContentsMargins(0, 0, 0, 0);
  camera_ = new CameraView(right);
  column->addWidget(camera_, 1);
  auto* centring = new QHBoxLayout;
  centre_ = new QCheckBox(tr("Centre holes"), right);
  centre_->setObjectName(QStringLiteral("centre_on_go"));
  centre_->setToolTip(tr("A click on a hole goes there and then centres it by eye"));
  centre_->setChecked(bridge_.can_centre());
  centre_->setEnabled(bridge_.can_centre());
  centring->addWidget(centre_);
  autocenter_ = button(tr("Autocenter"), "autocenter", right);
  autocenter_->setToolTip(tr("Centre the hole the stage was last sent to"));
  centring->addWidget(autocenter_);
  snapshot_ = button(tr("Snapshot"), "snapshot", right);
  snapshot_->setToolTip(tr("Save what the camera sees to the lab's snapshots"));
  centring->addWidget(snapshot_);
  outcome_ = new QLabel(right);
  outcome_->setObjectName(QStringLiteral("autocenter_outcome"));
  outcome_->setWordWrap(true);
  centring->addWidget(outcome_, 1);
  column->addLayout(centring);
  auto* tabs = new QTabWidget(right);
  tabs->setObjectName(QStringLiteral("tabs"));
  tabs->addTab(build_control(), tr("Control"));
  tabs->addTab(build_calibration(), tr("Calibration"));
  tabs->addTab(build_patterns(), tr("Patterns"));
  column->addWidget(tabs);
  split->addWidget(right);
  split->setStretchFactor(0, 3);
  split->setStretchFactor(1, 2);
  outer->addWidget(split, 1);
  setCentralWidget(central);

  status_ = new QLabel(this);
  status_->setObjectName(QStringLiteral("status"));
  statusBar()->addWidget(status_, 1);
  activity_ = new QLabel(this);
  activity_->setObjectName(QStringLiteral("activity"));
  statusBar()->addPermanentWidget(activity_);

  connect(tray_choice_, &QComboBox::activated, this,
          [this](int index) { bridge_.set_tray(tray_choice_->itemData(index).toString()); });
  connect(estop_, &QPushButton::clicked, this, [this] { bridge_.emergency_stop(); });
  connect(reset_, &QPushButton::clicked, this, [this] { bridge_.reset_stop(); });
  connect(tray_, &TrayView::holeClicked, this, [this](const QString& hole) {
    if (bridge_.watch_only() || bridge_.state().stopped) return;
    bridge_.go_to(hole, centre_->isChecked());
  });
  connect(tray_, &TrayView::holeMenu, this, [this](const QString& hole, const QPoint& where) {
    if (bridge_.watch_only() || bridge_.state().stopped) return;
    QMenu menu(this);
    menu.addAction(tr("Go to hole %1").arg(hole), this, [this, hole] { bridge_.go_to(hole, false); });
    if (bridge_.has_camera()) {
      menu.addAction(tr("Go to hole %1 and centre it").arg(hole), this, [this, hole] { bridge_.go_to(hole, true); });
    }
    menu.addSeparator();
    menu.addAction(tr("The stage is on hole %1 now: calibration point").arg(hole), this,
                   [this, hole] { bridge_.add_calibration_point(hole); });
    menu.exec(where);
  });
  connect(snapshot_, &QPushButton::clicked, this, [this] { bridge_.snapshot_to_file(); });
  connect(&bridge_, &LaserBridge::snapshotSaved, this, [this](const QString& file) { last_snapshot_ = file; });
  connect(autocenter_, &QPushButton::clicked, this, [this] {
    const QString hole = QString::fromStdString(bridge_.state().last_hole);
    if (!hole.isEmpty()) bridge_.go_to(hole, true);
  });
}

QWidget* LaserWindow::build_control() {
  auto* page = new QWidget(this);
  auto* row = new QHBoxLayout(page);

  auto* stage = new QGroupBox(tr("Stage"), page);
  auto* s = new QVBoxLayout(stage);
  position_ = new QLabel(stage);
  position_->setObjectName(QStringLiteral("position"));
  position_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  s->addWidget(position_);
  auto* pad = new QGridLayout;
  const auto jog_button = [this, stage, pad](const QString& text, const char* name, int r, int c, double dx, double dy,
                                             double dz) {
    auto* b = button(text, name, stage);
    b->setFixedWidth(44);
    pad->addWidget(b, r, c);
    jogs_.push_back(b);
    connect(b, &QPushButton::clicked, this, [this, dx, dy, dz] { jog(dx, dy, dz); });
  };
  jog_button(QStringLiteral("▲"), "jog_up", 0, 1, 0, 1, 0);
  jog_button(QStringLiteral("◀"), "jog_left", 1, 0, -1, 0, 0);
  jog_button(QStringLiteral("▶"), "jog_right", 1, 2, 1, 0, 0);
  jog_button(QStringLiteral("▼"), "jog_down", 2, 1, 0, -1, 0);
  jog_button(tr("Z+"), "jog_z_up", 0, 4, 0, 0, 1);
  jog_button(tr("Z−"), "jog_z_down", 2, 4, 0, 0, -1);
  pad->setColumnMinimumWidth(3, 12);
  s->addLayout(pad);
  auto* stepping = new QHBoxLayout;
  stepping->addWidget(new QLabel(tr("Step"), stage));
  step_ = new QDoubleSpinBox(stage);
  step_->setObjectName(QStringLiteral("jog_step"));
  step_->setDecimals(3);
  step_->setRange(0.001, 10);
  step_->setSingleStep(0.05);
  step_->setValue(0.1);
  step_->setSuffix(tr(" mm"));
  stepping->addWidget(step_);
  stepping->addStretch(1);
  stop_stage_ = button(tr("Stop stage"), "stop_stage", stage);
  stepping->addWidget(stop_stage_);
  s->addLayout(stepping);
  s->addStretch(1);
  row->addWidget(stage);

  auto* laser = new QGroupBox(tr("Laser"), page);
  auto* l = new QVBoxLayout(laser);
  enable_ = button(tr("Enable"), "enable", laser);
  enable_->setCheckable(true);
  l->addWidget(enable_);
  auto* out = new QHBoxLayout;
  out->addWidget(new QLabel(tr("Output"), laser));
  output_ = new QDoubleSpinBox(laser);
  output_->setObjectName(QStringLiteral("output"));
  output_->setDecimals(1);
  output_->setRange(0, 100);
  output_->setSuffix(tr(" %"));
  out->addWidget(output_, 1);
  l->addLayout(out);
  auto* firing = new QHBoxLayout;
  fire_ = button(tr("Fire"), "fire", laser);
  fire_->setToolTip(tr("Set the output and open the beam (the laser must be enabled)"));
  firing->addWidget(fire_);
  stop_beam_ = button(tr("Stop"), "stop_beam", laser);
  stop_beam_->setToolTip(tr("Output 0 and the beam closed"));
  firing->addWidget(stop_beam_);
  l->addLayout(firing);
  beam_ = new QLabel(laser);
  beam_->setObjectName(QStringLiteral("beam"));
  l->addWidget(beam_);
  interlocks_ = new QLabel(laser);
  interlocks_->setObjectName(QStringLiteral("interlocks"));
  interlocks_->setWordWrap(true);
  l->addWidget(interlocks_);
  l->addStretch(1);
  row->addWidget(laser);

  connect(stop_stage_, &QPushButton::clicked, this, [this] { bridge_.stop_stage(); });
  connect(enable_, &QPushButton::clicked, this, [this](bool on) { bridge_.enable(on); });
  connect(fire_, &QPushButton::clicked, this, [this] { bridge_.fire(output_->value()); });
  connect(stop_beam_, &QPushButton::clicked, this, [this] { bridge_.stop_beam(); });
  // While the beam is on, Enter in the box sends the output. Only Enter: a
  // value half typed is not sent because the focus went elsewhere.
  if (auto* typed = output_->findChild<QLineEdit*>()) {
    connect(typed, &QLineEdit::returnPressed, this, [this] {
      output_->interpretText();
      if (bridge_.state().firing.value_or(false) && !bridge_.watch_only()) bridge_.set_output(output_->value());
    });
  }
  return page;
}

QWidget* LaserWindow::build_calibration() {
  auto* page = new QWidget(this);
  auto* v = new QVBoxLayout(page);
  cal_table_ = table({tr("Hole"), tr("Stage x (mm)"), tr("Stage y (mm)")}, "cal_table", page);
  v->addWidget(cal_table_, 1);
  auto* row = new QHBoxLayout;
  row->addWidget(new QLabel(tr("The stage is on hole"), page));
  cal_hole_ = new QComboBox(page);
  cal_hole_->setObjectName(QStringLiteral("cal_hole"));
  row->addWidget(cal_hole_);
  cal_add_ = button(tr("Set point"), "cal_add", page);
  cal_add_->setToolTip(tr("Drive the stage until the hole is under the aim point, then set its point here"));
  row->addWidget(cal_add_);
  row->addStretch(1);
  cal_remove_ = button(tr("Remove"), "cal_remove", page);
  row->addWidget(cal_remove_);
  cal_clear_ = button(tr("Clear"), "cal_clear", page);
  row->addWidget(cal_clear_);
  v->addLayout(row);
  cal_solution_ = new QLabel(page);
  cal_solution_->setObjectName(QStringLiteral("cal_solution"));
  cal_solution_->setWordWrap(true);
  v->addWidget(cal_solution_);
  auto* scale_row = new QHBoxLayout;
  measure_scale_ = button(tr("Measure camera scale"), "measure_scale", page);
  measure_scale_->setToolTip(tr("With a hole under the aim point: jog the stage a step in x and in y and read the "
                                "camera's pixels per millimetre and flips off what the picture does"));
  scale_row->addWidget(measure_scale_);
  camera_scale_ = new QLabel(page);
  camera_scale_->setObjectName(QStringLiteral("camera_scale"));
  camera_scale_->setWordWrap(true);
  scale_row->addWidget(camera_scale_, 1);
  v->addLayout(scale_row);
  connect(measure_scale_, &QPushButton::clicked, this, [this] { bridge_.measure_scale(0.5); });
  connect(&bridge_, &LaserBridge::scaleMeasured, this, [this](const laser::ScaleMeasurement& m) {
    camera_scale_->setText(tr("%1 px/mm · flip x %2 · flip y %3 · saved")
                               .arg(mm(m.px_per_mm, 2), m.flip_x ? tr("yes") : tr("no"), m.flip_y ? tr("yes") : tr("no")));
  });
  cal_cautions_ = new QLabel(page);
  cal_cautions_->setObjectName(QStringLiteral("cal_cautions"));
  cal_cautions_->setWordWrap(true);
  style::set_tone(cal_cautions_, style::Tone::Warning);
  v->addWidget(cal_cautions_);

  connect(cal_add_, &QPushButton::clicked, this, [this] {
    if (!cal_hole_->currentText().isEmpty()) bridge_.add_calibration_point(cal_hole_->currentText());
  });
  connect(cal_remove_, &QPushButton::clicked, this, [this] {
    const int row_index = cal_table_->currentRow();
    if (row_index >= 0) bridge_.remove_calibration_point(cal_table_->item(row_index, 0)->text());
  });
  connect(cal_clear_, &QPushButton::clicked, this, [this] { bridge_.clear_calibration(); });
  connect(cal_table_, &QTableWidget::itemSelectionChanged, this, [this] { refresh_enabled(); });
  return page;
}

QWidget* LaserWindow::build_patterns() {
  auto* page = new QWidget(this);
  auto* v = new QVBoxLayout(page);
  pattern_list_ = table({tr("Pattern"), tr("Kind"), tr("Length"), tr("Time")}, "pattern_list", page);
  v->addWidget(pattern_list_, 1);
  auto* row = new QHBoxLayout;
  pattern_run_ = button(tr("Run"), "pattern_run", page);
  pattern_run_->setToolTip(tr("Run the pattern about where the stage is now"));
  row->addWidget(pattern_run_);
  pattern_stop_ = button(tr("Stop"), "pattern_stop", page);
  row->addWidget(pattern_stop_);
  row->addStretch(1);
  pattern_maker_ = button(tr("Pattern maker…"), "pattern_maker", page);
  pattern_maker_->hide();
  row->addWidget(pattern_maker_);
  v->addLayout(row);

  connect(pattern_run_, &QPushButton::clicked, this, [this] {
    if (const QString name = selected_pattern(); !name.isEmpty()) bridge_.run_pattern(name);
  });
  connect(pattern_stop_, &QPushButton::clicked, this, [this] { bridge_.stop_pattern(); });
  connect(pattern_maker_, &QPushButton::clicked, this, [this] {
    if (open_pattern_maker_) open_pattern_maker_();
  });
  connect(pattern_list_, &QTableWidget::itemSelectionChanged, this, [this] { refresh_enabled(); });
  return page;
}

void LaserWindow::set_pattern_maker(std::function<void()> open) {
  open_pattern_maker_ = std::move(open);
  pattern_maker_->setVisible(static_cast<bool>(open_pattern_maker_));
}

QString LaserWindow::selected_pattern() const {
  const int row = pattern_list_->currentRow();
  return row >= 0 ? pattern_list_->item(row, 0)->text() : QString();
}

void LaserWindow::refresh_patterns() {
  const QString keep = selected_pattern();
  const laser::PatternLibrary& library = bridge_.patterns();
  const auto names = library.names();
  pattern_list_->setRowCount(0);
  for (const auto& name : names) {
    const auto pattern = library.find(name);
    if (pattern == nullptr) continue;
    const int row = pattern_list_->rowCount();
    pattern_list_->insertRow(row);
    auto* first = new QTableWidgetItem(QString::fromStdString(name));
    // Whether it can be run by hand rides with the row.
    first->setData(Qt::UserRole, pattern->follows_glow());
    pattern_list_->setItem(row, 0, first);
    pattern_list_->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(std::string(to_string(pattern->kind)))));
    QString length = tr("follows the glow"), time = tr("the run's duration");
    if (pattern->follows_glow()) {
      first->setToolTip(tr("A dragonfly follows the glow of a heated sample: it runs from a queue"));
    } else if (const auto path = laser::pattern_path(*pattern, pattern->seed.value_or(0))) {
      const double total = laser::path_length(*path);
      length = tr("%1 mm").arg(mm(total, 2));
      time = tr("%1 s").arg(mm(total / pattern->velocity, 1));
    } else {
      length = QString::fromStdString(path.error().what);
      time.clear();
    }
    pattern_list_->setItem(row, 2, new QTableWidgetItem(length));
    pattern_list_->setItem(row, 3, new QTableWidgetItem(time));
    if (QString::fromStdString(name) == keep) pattern_list_->selectRow(row);
  }
  pattern_list_->resizeColumnsToContents();
  refresh_enabled();
}

void LaserWindow::jog(double dx, double dy, double dz) {
  const double step = step_->value();
  bridge_.jog(dx * step, dy * step, dz * step);
}

void LaserWindow::refresh_tray() {
  const QString tray = QString::fromStdString(bridge_.state().tray);
  shown_tray_ = tray;
  {
    const QSignalBlocker quiet(tray_choice_);
    const int index = tray_choice_->findData(tray);
    tray_choice_->setCurrentIndex(index < 0 ? 0 : index);
  }
  const laser::TrayMap* map = tray.isEmpty() ? nullptr : bridge_.tray_map(tray);
  tray_->set_tray(map);
  const QString keep = cal_hole_->currentText();
  cal_hole_->clear();
  cal_table_->setRowCount(0);
  cal_solution_->clear();
  cal_cautions_->clear();
  tray_->set_calibration_holes({});
  tray_->set_corrected_holes(bridge_.corrected_holes());
  tray_->set_transform(std::nullopt);
  if (map == nullptr) {
    cal_solution_->setText(tr("No tray is set."));
    refresh_enabled();
    return;
  }
  for (const auto& hole : map->holes()) cal_hole_->addItem(QString::fromStdString(hole.id));
  // The map's own centre hole is where a calibration starts.
  QString preferred = keep;
  if (preferred.isEmpty() && map->center_hole()) preferred = QString::fromStdString(*map->center_hole());
  if (const int index = cal_hole_->findText(preferred); index >= 0) cal_hole_->setCurrentIndex(index);

  const auto points = bridge_.calibration_points(tray);
  QStringList holes;
  for (const auto& point : points) {
    const int row = cal_table_->rowCount();
    cal_table_->insertRow(row);
    cal_table_->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(point.hole)));
    cal_table_->setItem(row, 1, new QTableWidgetItem(mm(point.x)));
    cal_table_->setItem(row, 2, new QTableWidgetItem(mm(point.y)));
    holes.push_back(QString::fromStdString(point.hole));
  }
  tray_->set_calibration_holes(holes);
  const laser::CalibrationStatus status = bridge_.calibration(tray);
  if (status.solution) {
    const laser::Transform& t = status.solution->transform;
    tray_->set_transform(t);
    cal_solution_->setText(tr("Centre %1, %2 mm · rotation %3° · scale %4 · rms %5 mm · %6 point(s)")
                               .arg(mm(t.cx), mm(t.cy), mm(t.rotation * 180.0 / std::numbers::pi, 2), mm(t.scale, 4),
                                    mm(status.solution->rms_mm))
                               .arg(status.solution->points));
    QStringList said;
    for (const auto& caution : laser::cautions(*map, points, *status.solution)) said.push_back(QString::fromStdString(caution));
    cal_cautions_->setText(said.join(QLatin1Char('\n')));
  } else {
    cal_solution_->setText(QString::fromStdString(status.why.empty() ? std::string("not calibrated") : status.why));
  }
  refresh_enabled();
}

void LaserWindow::on_snapshot(const laser::LaserSnapshot& s) {
  if (QString::fromStdString(s.tray) != shown_tray_ || (tray_->tray().isEmpty() && !s.tray.empty())) refresh_tray();
  if (s.position) {
    position_->setText(tr("x %1   y %2   z %3 mm").arg(mm(s.position->x), mm(s.position->y), mm(s.position->z)));
    tray_->set_stage(laser::StageXY{s.position->x, s.position->y});
  } else {
    position_->setText(tr("position unknown"));
    tray_->set_stage(std::nullopt);
  }
  tray_->set_current_hole(QString::fromStdString(s.last_hole));
  // A centring that just ended may have left a correction.
  tray_->set_corrected_holes(bridge_.corrected_holes());
  {
    const QSignalBlocker quiet(enable_);
    enable_->setChecked(s.enabled.value_or(false));
    enable_->setText(s.enabled.value_or(false) ? tr("Enabled") : tr("Enable"));
  }
  const bool firing = s.firing.value_or(false);
  beam_->setText(firing ? tr("FIRING at %1 %").arg(mm(s.output.value_or(0), 1))
                        : s.output.value_or(0) > 0 ? tr("beam off · output %1 %").arg(mm(*s.output, 1)) : tr("beam off"));
  style::set_tone(beam_, firing ? style::Tone::Error : style::Tone::Muted);
  if (s.interlocks.empty()) {
    interlocks_->setText(s.has_laser ? tr("interlocks: ok") : tr("no laser"));
    style::set_tone(interlocks_, style::Tone::Muted);
  } else {
    QStringList names;
    for (const auto& name : s.interlocks) names.push_back(QString::fromStdString(name));
    interlocks_->setText(tr("interlock tripped: %1").arg(names.join(QStringLiteral(", "))));
    style::set_tone(interlocks_, style::Tone::Error);
  }
  QString doing = QString::fromStdString(std::string(to_string(s.activity)));
  if (s.activity == laser::LaserActivity::Pattern) doing = tr("pattern %1").arg(QString::fromStdString(s.pattern_progress));
  if (s.activity == laser::LaserActivity::Centring) doing = tr("centring hole %1").arg(QString::fromStdString(s.last_hole));
  if (!s.error.empty()) doing += tr(" · %1").arg(QString::fromStdString(s.error));
  activity_->setText(doing);
  outcome_->setText(outcome_text(s.autocenter));
  refresh_enabled();
}

void LaserWindow::refresh_enabled() {
  const laser::LaserSnapshot& s = bridge_.state();
  const bool watching = bridge_.watch_only();
  const bool can_drive = !watching && !s.stopped;
  const bool busy = s.activity != laser::LaserActivity::Idle;

  if (s.stopped) {
    banner_->setText(watching ? tr("Emergency stop: everything is off. Reset once the queue has ended.")
                              : tr("Emergency stop: everything is off. Reset to drive again."));
    banner_->show();
  } else if (watching) {
    banner_->setText(tr("Queue running: watch only"));
    banner_->show();
  } else {
    banner_->hide();
  }
  // Not under a queue: it may not have seen its abort yet.
  reset_->setVisible(s.stopped && !watching);
  estop_->setEnabled(true);  // always

  tray_choice_->setEnabled(can_drive && !busy);
  tray_->setEnabled(can_drive);
  for (QPushButton* b : jogs_) b->setEnabled(can_drive && s.has_stage);
  step_->setEnabled(can_drive && s.has_stage);
  stop_stage_->setEnabled(!watching && s.has_stage);
  centre_->setEnabled(can_drive && bridge_.can_centre());
  autocenter_->setEnabled(can_drive && bridge_.can_centre() && !s.last_hole.empty());
  snapshot_->setEnabled(s.has_camera);  // looking is always allowed
  measure_scale_->setEnabled(can_drive && s.has_camera && s.has_stage && !busy);

  enable_->setEnabled(can_drive);
  output_->setEnabled(can_drive);
  // Enable, then fire: live only when the laser is enabled and nothing is tripped.
  fire_->setEnabled(can_drive && s.has_laser && s.enabled.value_or(false) && s.interlocks.empty());
  stop_beam_->setEnabled(!watching);

  const bool has_tray = !s.tray.empty();
  cal_hole_->setEnabled(can_drive && has_tray);
  cal_add_->setEnabled(can_drive && has_tray && cal_hole_->count() > 0);
  cal_remove_->setEnabled(can_drive && cal_table_->currentRow() >= 0);
  cal_clear_->setEnabled(can_drive && cal_table_->rowCount() > 0);

  const int row = pattern_list_->currentRow();
  const bool by_eye = row >= 0 && pattern_list_->item(row, 0)->data(Qt::UserRole).toBool();
  pattern_run_->setEnabled(can_drive && row >= 0 && !by_eye && !busy);
  pattern_run_->setToolTip(by_eye ? tr("A dragonfly follows the glow of a heated sample: it runs from a queue")
                                  : tr("Run the pattern about where the stage is now"));
  pattern_stop_->setEnabled(!watching);
}

void LaserWindow::closeEvent(QCloseEvent* event) {
  if (!bridge_.watch_only() && bridge_.state().firing.value_or(false)) bridge_.stop_beam();
  QMainWindow::closeEvent(event);
}

void LaserWindow::on_finished(const QString& what, const Result<void>& result) {
  QString name = what;
  name.replace(QLatin1Char('_'), QLatin1Char(' '));
  if (result) {
    // where the picture went is the news
    status_->setText(what == QStringLiteral("snapshot") && !last_snapshot_.isEmpty() ? tr("snapshot: %1").arg(last_snapshot_)
                                                                                    : tr("%1: done").arg(name));
    style::set_tone(status_, style::Tone::Muted);
  } else if (result.error().kind == ErrorKind::Cancelled) {
    status_->setText(tr("%1: stopped").arg(name));
    style::set_tone(status_, style::Tone::Warning);
  } else {
    status_->setText(tr("%1: %2").arg(name, QString::fromStdString(result.error().what)));
    style::set_tone(status_, style::Tone::Error);
  }
}

}  // namespace pychron::ui
