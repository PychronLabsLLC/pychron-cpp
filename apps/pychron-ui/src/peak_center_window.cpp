#include "peak_center_window.hpp"

#include <map>
#include <string>
#include <utility>

#include <QCloseEvent>
#include <QColor>
#include <QMessageBox>
#include <QScrollArea>
#include <QSplitter>
#include <QTimer>

#include "peak_center_panel.hpp"
#include "peak_center_view.hpp"

namespace pychron::ui {

PeakCenterWindow::PeakCenterWindow(SpectrometerBridge& bridge, bool simulation, std::unique_ptr<QSettings> settings,
                                   QWidget* parent)
    : QMainWindow(parent), bridge_(bridge), settings_(settings ? std::move(settings) : std::make_unique<QSettings>()) {
  setObjectName(QStringLiteral("PeakCenterWindow"));
  setWindowTitle(simulation ? tr("Peak Center (Simulation)") : tr("Peak Center"));
  resize(1100, 650);  // the saved geometry, if any, wins below

  // The plot's colours are the strip chart's: the config's colour, else the
  // detector's palette slot, else its place in the list.
  std::map<std::string, QColor> colors;
  std::map<std::string, std::size_t> palette_slots;
  {
    const auto detectors = bridge_.detectors();
    for (std::size_t i = 0; i < detectors.size(); ++i) {
      if (detectors[i].palette_slot) {
        palette_slots[detectors[i].name] = *detectors[i].palette_slot;
      } else if (detectors[i].color.isValid()) {
        colors[detectors[i].name] = detectors[i].color;
      } else {
        palette_slots[detectors[i].name] = i;
      }
    }
  }
  panel_ = new PeakCenterPanel(bridge_);
  view_ = new PeakCenterView(std::move(colors), std::move(palette_slots));
  splitter_ = new QSplitter(Qt::Horizontal);
  splitter_->setObjectName(QStringLiteral("peakCenterSplitter"));
  splitter_->setChildrenCollapsible(false);
  // The form is taller than a laptop's screen: it scrolls, the plot does not.
  auto* scroll = new QScrollArea;
  scroll->setObjectName(QStringLiteral("peakCenterPanelScroll"));
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll->setWidget(panel_);
  scroll->setMinimumWidth(panel_->sizeHint().width());
  splitter_->addWidget(scroll);
  splitter_->addWidget(view_);
  splitter_->setStretchFactor(0, 0);
  splitter_->setStretchFactor(1, 1);
  setCentralWidget(splitter_);

  // Saved state is untrusted: what Qt refuses leaves the defaults.
  if (const auto g = settings_->value(key("geometry")).toByteArray(); !g.isEmpty()) restoreGeometry(g);
  if (const auto s = settings_->value(key("splitter")).toByteArray(); !s.isEmpty()) splitter_->restoreState(s);

  connect(panel_, &PeakCenterPanel::startRequested, view_, [this] { view_->clear(); });
  connect(&bridge_, &SpectrometerBridge::peakCenterTryStarted, view_, &PeakCenterView::on_try_started);
  connect(&bridge_, &SpectrometerBridge::peakCenterPoint, view_, &PeakCenterView::on_point);
  connect(&bridge_, &SpectrometerBridge::peakCenterFinished, this, [this](const Result<jobs::PeakCenterResult>& r) {
    if (r) view_->on_done(jobs::PeakCenterDone{*r});  // a failed fit too; an error has no result
    if (closing_after_run_ && !bridge_.peak_center_running()) {
      closing_after_run_ = false;
      QTimer::singleShot(0, this, [this] { close(); });
    }
  });
}

QString PeakCenterWindow::key(const char* name) const {
  return QStringLiteral("peak_center_window/%1/%2").arg(bridge_.name(), QLatin1String(name));
}

void PeakCenterWindow::set_peak_center_file(const std::filesystem::path& file) { panel_->set_file(file); }

void PeakCenterWindow::set_queue_running(std::function<bool()> queue_running) {
  panel_->set_queue_running(std::move(queue_running));
}

void PeakCenterWindow::queue_state_changed() { panel_->queue_state_changed(); }

void PeakCenterWindow::closeEvent(QCloseEvent* event) {
  if (bridge_.peak_center_running()) {
    // The run ends through its own finish: close again when it has. A second
    // close while waiting is not asked again.
    if (!closing_after_run_) {
      const QString question = tr("Cancel peak center?");
      const bool yes = ask_ ? ask_(question)
                            : QMessageBox::question(this, tr("Peak center"), question,
                                                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No) ==
                                  QMessageBox::Yes;
      if (yes) {
        closing_after_run_ = true;
        // The spectrometer window stays open: its scan goes back as it was.
        bridge_.cancel_peak_center(SpectrometerBridge::ScanAfter::Resume);
      }
    }
    event->ignore();
    return;
  }
  settings_->setValue(key("geometry"), saveGeometry());
  settings_->setValue(key("splitter"), splitter_->saveState());
  settings_->sync();
  QMainWindow::closeEvent(event);
}

}  // namespace pychron::ui
