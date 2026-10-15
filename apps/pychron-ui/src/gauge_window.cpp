#include "gauge_window.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QHideEvent>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QShowEvent>
#include <QStringList>
#include <QVBoxLayout>

#include "history_chart.hpp"

namespace pychron::ui {

namespace {

const QString kWallFormat = QStringLiteral("yyyy-MM-dd HH:mm:ss");

// As the gauge's chip writes a reading.
QString chip_number(double value) { return QString::number(value, 'e', 2); }

QString range_label(std::chrono::seconds range) {
  if (range < std::chrono::hours(1))
    return QObject::tr("%1 min").arg(std::chrono::duration_cast<std::chrono::minutes>(range).count());
  return QObject::tr("%1 h").arg(std::chrono::duration_cast<std::chrono::hours>(range).count());
}

qint64 ms_of(Duration d) { return std::chrono::duration_cast<std::chrono::milliseconds>(d).count(); }

}  // namespace

const std::vector<std::chrono::seconds>& GaugeWindow::ranges() {
  using std::chrono::seconds;
  static const std::vector<seconds> kRanges{seconds(600), seconds(1800), seconds(3600), seconds(21600),
                                            seconds(86400)};
  return kRanges;
}

GaugeWindow::GaugeWindow(QString name, Deps deps, QWidget* parent)
    : QWidget(parent, Qt::Window),
      name_(std::move(name)),
      deps_(std::move(deps)),
      header_(new QLabel),
      range_box_(new QComboBox),
      log_box_(new QCheckBox(tr("Log"))),
      chart_(new HistoryChart) {
  setAttribute(Qt::WA_DeleteOnClose, false);
  setWindowTitle(name_);
  resize(560, 340);

  for (const auto range : ranges()) range_box_->addItem(range_label(range), static_cast<qlonglong>(range.count()));
  auto* copy = new QPushButton(tr("Copy"));

  auto* top = new QHBoxLayout;
  top->addWidget(header_, 1);
  top->addWidget(range_box_);
  top->addWidget(log_box_);
  top->addWidget(copy);
  auto* layout = new QVBoxLayout(this);
  layout->addLayout(top);
  layout->addWidget(chart_, 1);

  // What was kept for this gauge; a range that is none of those offered is the default.
  if (deps_.settings) {
    const auto settings = deps_.settings();
    const qlonglong kept =
        settings->value(key(QStringLiteral("range_s")), static_cast<qlonglong>(kDefaultRange.count())).toLongLong();
    const int at = range_box_->findData(kept);
    range_box_->setCurrentIndex(at >= 0 ? at : 0);
    log_box_->setChecked(settings->value(key(QStringLiteral("log")), false).toBool());
    if (const QByteArray g = settings->value(key(QStringLiteral("geometry"))).toByteArray(); !g.isEmpty())
      restoreGeometry(g);
  }

  std::vector<double> guides;
  if (deps_.alarm_low) guides.push_back(*deps_.alarm_low);
  if (deps_.alarm_high) guides.push_back(*deps_.alarm_high);
  chart_->set_guides(std::move(guides));
  chart_->set_log(log_box_->isChecked());

  deferred_.setSingleShot(true);
  connect(&deferred_, &QTimer::timeout, this, &GaugeWindow::draw);
  connect(range_box_, &QComboBox::currentIndexChanged, this, [this] {
    save(QStringLiteral("range_s"), range_box_->currentData());
    draw();
  });
  connect(log_box_, &QCheckBox::toggled, this, [this](bool log) {
    save(QStringLiteral("log"), log);
    chart_->set_log(log);
  });
  connect(copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(copy_text()); });

  draw();
}

std::chrono::seconds GaugeWindow::range() const {
  return std::chrono::seconds(range_box_->currentData().toLongLong());
}

void GaugeWindow::refresh() {
  // A hidden window (closed, kept by the main window) draws when it is shown again.
  if (!isVisible()) {
    stale_ = true;
    return;
  }
  if (!since_draw_.isValid() || since_draw_.elapsed() >= kMinRedrawMs) {
    draw();
  } else if (!deferred_.isActive()) {
    deferred_.start(kMinRedrawMs - static_cast<int>(since_draw_.elapsed()));
  }
}

void GaugeWindow::draw() {
  deferred_.stop();
  stale_ = false;
  ++redraws_;
  // The cap is measured from the end of a drawing: a slow one does not let
  // the next follow at once.
  const auto done = qScopeGuard([this] { since_draw_.start(); });
  if (deps_.history == nullptr) return;
  const std::string name = name_.toStdString();
  drawn_revision_ = deps_.history->revision(name);
  shown_ = deps_.history->series(name, Duration(range()));
  // The wall clock is asked once, for the newest reading: every other time
  // is that less its age, so a jump of a simulated clock between two asks
  // cannot set two points of one drawing apart.
  newest_wall_ = shown_.newest && deps_.wall_of ? deps_.wall_of(*shown_.newest) : QDateTime();
  chart_->set_series(shown_, range(), newest_wall_);
  update_header();
}

void GaugeWindow::update_header() {
  const auto summary = chart_->summary();
  if (!summary) {
    header_->setText(tr("No readings"));
    return;
  }
  QString text = QStringLiteral("%1 %2   min %3   max %4")
                     .arg(chip_number(summary->latest), shown_.units, chip_number(summary->lowest),
                          chip_number(summary->highest));
  // A range longer than what the history holds: since when it holds readings.
  const auto oldest = deps_.history->oldest(name_.toStdString());
  if (oldest && shown_.newest && *shown_.newest - *oldest < Duration(range())) {
    const Duration held = *shown_.newest - *oldest;
    if (newest_wall_.isValid()) {
      text += QStringLiteral("   since %1").arg(newest_wall_.addMSecs(-ms_of(held)).toString(QStringLiteral("hh:mm")));
    } else {
      const double minutes = static_cast<double>(ms_of(held)) / 60000.0;
      text += tr("   since %1 min before newest").arg(QString::number(minutes, 'g', 3));
    }
  }
  header_->setText(text);
}

QString GaugeWindow::header() const { return header_->text(); }

QString GaugeWindow::copy_text() const {
  const QString value_title = shown_.units.isEmpty() ? tr("value") : tr("value (%1)").arg(shown_.units);
  QStringList lines;
  lines << (newest_wall_.isValid() ? tr("time") : tr("seconds before newest")) + '\t' + value_title;
  for (const auto& p : shown_.points) {
    // Every reading of the range, a non-positive one on a log scale too: the
    // chart leaves that out, the record does not.
    if (!std::isfinite(p.value)) continue;  // a break
    const QString time = newest_wall_.isValid()
                             ? newest_wall_.addMSecs(std::llround(p.t * 1000.0)).toString(kWallFormat)
                             : QString::number(p.t, 'g', QLocale::FloatingPointShortest);
    // Full precision: the shortest text that reads back as the same double.
    lines << time + '\t' + QString::number(p.value, 'g', QLocale::FloatingPointShortest);
  }
  return lines.join('\n') + '\n';
}

void GaugeWindow::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  if (stale_) draw();
}

void GaugeWindow::hideEvent(QHideEvent* event) {
  // Closed, or hidden with the main window: where it was is kept either way.
  save(QStringLiteral("geometry"), saveGeometry());
  QWidget::hideEvent(event);
}

QString GaugeWindow::key(const QString& leaf) const {
  return QStringLiteral("gauge_window/%1/%2/%3").arg(deps_.line, name_, leaf);
}

void GaugeWindow::save(const QString& leaf, const QVariant& value) const {
  if (!deps_.settings) return;
  deps_.settings()->setValue(key(leaf), value);
}

}  // namespace pychron::ui
