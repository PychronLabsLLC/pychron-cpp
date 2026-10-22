#include "rise_rate_panel.hpp"

#include <fstream>
#include <system_error>
#include <utility>

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "pychron/core/user_file.hpp"

namespace pychron::ui {

namespace {

QString number(double v, int figures = 4) { return QString::number(v, 'g', figures); }

// One CSV field (RFC 4180): quoted when it holds a comma, a quote or a line break.
std::string csv(const QString& text) {
  QString out = text;
  if (out.contains(QLatin1Char(',')) || out.contains(QLatin1Char('"')) || out.contains(QLatin1Char('\n')) ||
      out.contains(QLatin1Char('\r'))) {
    out.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    out = QLatin1Char('"') + out + QLatin1Char('"');
  }
  return out.toStdString();
}

constexpr const char* kFileHeader =
    "time,spectrometer,simulated,detector,isotope,endpoints,linear,linear_se,units,minutes,n,note\n";

}  // namespace

RiseRatePanel::RiseRatePanel(std::vector<DetectorSeries> detectors, QWidget* parent)
    : QWidget(parent),
      detectors_(std::move(detectors)),
      detector_(new QComboBox),
      toggle_(new QPushButton(tr("Start"))),
      live_(new QLabel),
      status_(new QLabel),
      results_(new QTableWidget),
      copy_(new QPushButton(tr("Copy"))),
      clear_(new QPushButton(tr("Clear results"))) {
  for (const auto& detector : detectors_) {
    detector_->addItem(QString::fromStdString(detector.name));
    if (!detector.isotope.isEmpty()) isotopes_[detector.name] = detector.isotope;
  }
  toggle_->setEnabled(!detectors_.empty());
  toggle_->setToolTip(tr("Start with the spectrometer isolated; stop when the rate has settled. Nothing is sent to "
                         "the instrument."));
  live_->setWordWrap(true);
  status_->setWordWrap(true);

  results_->setColumnCount(ColCount);
  results_->setHorizontalHeaderLabels({tr("#"), tr("Detector"), tr("Endpoints"), tr("Linear"), tr("±"), tr("Units"),
                                       tr("Minutes"), tr("n"), tr("Time"), tr("Note")});
  results_->horizontalHeaderItem(ColEndpoints)->setToolTip(tr("Last reading less the first, over the time between them"));
  results_->horizontalHeaderItem(ColLinear)->setToolTip(tr("Slope of a straight line fitted to every reading"));
  results_->horizontalHeaderItem(ColError)->setToolTip(tr("Standard error of the fitted slope (1σ)"));
  results_->verticalHeader()->hide();
  results_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  results_->setSelectionBehavior(QAbstractItemView::SelectRows);
  results_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  results_->setMinimumHeight(110);
  copy_->setEnabled(false);
  clear_->setEnabled(false);

  auto* top = new QHBoxLayout;
  top->addWidget(detector_, 1);
  top->addWidget(toggle_);
  auto* bottom = new QHBoxLayout;
  bottom->addStretch();
  bottom->addWidget(copy_);
  bottom->addWidget(clear_);
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addLayout(top);
  layout->addWidget(live_);
  layout->addWidget(status_);
  layout->addWidget(results_, 1);
  layout->addLayout(bottom);

  connect(toggle_, &QPushButton::clicked, this, [this] { toggle(); });
  connect(copy_, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(copy_text()); });
  connect(clear_, &QPushButton::clicked, this, [this] {
    results_->setRowCount(0);
    count_ = 0;
    copy_->setEnabled(false);
    clear_->setEnabled(false);
  });
}

void RiseRatePanel::set_detector(const QString& name) {
  if (run_.running()) return;
  if (const int index = detector_->findText(name); index >= 0) detector_->setCurrentIndex(index);
}

void RiseRatePanel::set_isotope(const std::string& detector, const QString& isotope) { isotopes_[detector] = isotope; }

void RiseRatePanel::set_wall_clock(std::function<QDateTime()> now) { wall_now_ = std::move(now); }

void RiseRatePanel::set_results_file(std::filesystem::path path, QString spectrometer, bool simulated) {
  file_ = std::move(path);
  spectrometer_ = std::move(spectrometer);
  simulated_ = simulated;
}

bool RiseRatePanel::append_to_file(const RiseRate& rate, const QString& time, const QString& note) const {
  std::error_code ec;
  if (file_.has_parent_path()) std::filesystem::create_directories(file_.parent_path(), ec);
  const bool fresh = !std::filesystem::exists(file_, ec) || std::filesystem::file_size(file_, ec) == 0;
  std::ofstream out(file_, std::ios::app | std::ios::binary);
  if (!out) return false;
  if (fresh) out << kFileHeader;
  const auto isotope = isotopes_.find(run_.detector());
  // The file keeps the numbers whole; the table rounds them for the eye.
  out << csv(time) << ',' << csv(spectrometer_) << ',' << (simulated_ ? "yes" : "") << ','
      << csv(QString::fromStdString(run_.detector())) << ',' << csv(isotope == isotopes_.end() ? QString() : isotope->second)
      << ',' << number(rate.endpoints, 10).toStdString() << ',' << number(rate.linear, 10).toStdString() << ','
      << (rate.linear_se ? number(*rate.linear_se, 10).toStdString() : std::string()) << ','
      << csv(units_of(run_.detector())) << ',' << number(rate.duration_min, 10).toStdString() << ',' << rate.n << ','
      << csv(note) << '\n';
  out.flush();
  if (!out) return false;
  out.close();
  if (fresh) pychron::mark_as_user_file(file_);
  return true;
}

QString RiseRatePanel::units_of(const std::string& detector) const {
  for (const auto& d : detectors_)
    if (d.name == detector) return d.units.isEmpty() ? tr("/min") : tr("%1/min").arg(d.units);
  return tr("/min");
}

void RiseRatePanel::set_start_allowed(bool allowed) {
  start_allowed_ = allowed;
  toggle_->setEnabled(!detectors_.empty() && (start_allowed_ || run_.running()));
}

void RiseRatePanel::toggle() {
  if (!run_.running()) {
    if (!start_allowed_ || detector_->currentIndex() < 0) return;
    run_.start(detector_->currentText().toStdString());
    announced_ = false;
    void_said_ = false;
    detector_->setEnabled(false);
    toggle_->setText(tr("Stop"));
    status_->clear();
    emit cleared();
    show_live();
    return;
  }
  run_.stop();
  detector_->setEnabled(true);
  toggle_->setText(tr("Start"));
  set_start_allowed(start_allowed_);
  const auto rate = run_.current();
  if (run_.restarted()) {
    status_->setText(tr("No result: the readings began again while it ran (the scan was restarted)."));
  } else if (!rate) {
    status_->setText(tr("No result: fewer than two readings."));
    emit cleared();
  } else {
    add_result(*rate);
    emit stopped(*run_.last());
  }
  live_->clear();
}

void RiseRatePanel::abandon(const QString& why) {
  if (!run_.running()) return;
  run_.stop();
  detector_->setEnabled(true);
  toggle_->setText(tr("Start"));
  set_start_allowed(start_allowed_);
  live_->clear();
  status_->setText(tr("No result: %1.").arg(why));
  emit cleared();
}

void RiseRatePanel::on_reading(const spectrometer::IntensityReading& reading) {
  if (!run_.running()) return;
  run_.add(reading);
  if (!announced_ && run_.first()) {
    announced_ = true;
    emit started(*run_.first());
  }
  if (run_.restarted() && !void_said_) {
    void_said_ = true;
    emit cleared();  // the chart's time began again: the start mark means nothing now
  }
  show_live();
}

void RiseRatePanel::show_live() {
  if (run_.restarted()) {
    live_->setText(tr("The readings began again: stop, and start a new run."));
    return;
  }
  const auto rate = run_.current();
  if (!rate) {
    live_->setText(run_.first() ? tr("1 reading…") : tr("Waiting for a reading…"));
    return;
  }
  const QString units = units_of(run_.detector());
  QString linear = number(rate->linear);
  if (rate->linear_se) linear += tr(" ± %1").arg(number(*rate->linear_se, 2));
  live_->setText(tr("%1 readings, %2 min: endpoints %3, linear %4 %5")
                     .arg(rate->n)
                     .arg(number(rate->duration_min, 3), number(rate->endpoints), linear, units));
}

void RiseRatePanel::add_result(const RiseRate& rate) {
  const int row = results_->rowCount();
  results_->insertRow(row);
  const auto put = [this, row](int column, const QString& text) {
    results_->setItem(row, column, new QTableWidgetItem(text));  // the table owns its items
  };
  QString detector = QString::fromStdString(run_.detector());
  if (const auto it = isotopes_.find(run_.detector()); it != isotopes_.end() && !it->second.isEmpty())
    detector = tr("%1 (%2)").arg(detector, it->second);
  put(ColNumber, QString::number(++count_));
  put(ColDetector, detector);
  put(ColEndpoints, number(rate.endpoints));
  put(ColLinear, number(rate.linear));
  put(ColError, rate.linear_se ? number(*rate.linear_se, 2) : QString());
  put(ColUnits, units_of(run_.detector()));
  put(ColMinutes, number(rate.duration_min, 3));
  put(ColReadings, QString::number(rate.n));
  const QString time = wall_now_ ? wall_now_().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : QString();
  const QString note = run_.saturated() ? tr("saturated") : QString();
  put(ColTime, time);
  put(ColNote, note);
  if (!file_.empty() && !append_to_file(rate, time, note))
    status_->setText(tr("The result is in the table, but could not be written to %1.")
                         .arg(QString::fromStdString(file_.string())));
  results_->scrollToBottom();
  copy_->setEnabled(true);
  clear_->setEnabled(true);
}

QString RiseRatePanel::live_text() const { return live_->text(); }

QString RiseRatePanel::status_text() const { return status_->text(); }

QString RiseRatePanel::copy_text() const {
  QStringList lines;
  QStringList header;
  for (int c = 0; c < ColCount; ++c) header << results_->horizontalHeaderItem(c)->text();
  lines << header.join(QLatin1Char('\t'));
  for (int r = 0; r < results_->rowCount(); ++r) {
    QStringList cells;
    for (int c = 0; c < ColCount; ++c) cells << results_->item(r, c)->text();
    lines << cells.join(QLatin1Char('\t'));
  }
  return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

}  // namespace pychron::ui
