#include "gauge_popup.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include <QEvent>
#include <QGuiApplication>
#include <QLabel>
#include <QScreen>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "history_chart.hpp"

namespace pychron::ui {

namespace {

constexpr int kGap = 8;  // between the chip and the popup

}  // namespace

GaugePopup::GaugePopup(const ReadingHistory& history, std::function<TimePoint()> now,
                       std::function<QDateTime(TimePoint)> wall_of, QWidget* parent)
    : QFrame(parent, Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowTransparentForInput),
      history_(history),
      now_(std::move(now)),
      wall_of_(std::move(wall_of)),
      heading_(new QLabel),
      stack_(new QStackedWidget),
      chart_(new HistoryChart),
      message_(new QLabel) {
  setAttribute(Qt::WA_TransparentForMouseEvents);
  setAttribute(Qt::WA_ShowWithoutActivating);
  setFocusPolicy(Qt::NoFocus);
  setFrameShape(QFrame::Box);
  setFixedSize(kSize);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(6, 4, 6, 4);
  layout->addWidget(heading_);
  layout->addWidget(stack_, 1);
  chart_->set_compact(true);
  chart_->set_log(false);
  message_->setAlignment(Qt::AlignCenter);
  stack_->addWidget(chart_);
  stack_->addWidget(message_);

  if (parent != nullptr) parent->installEventFilter(this);
  connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
    if (state != Qt::ApplicationActive) hide();
  });
}

QRect GaugePopup::placed(const QRect& chip, const QSize& size, const QRect& screen) {
  // std::clamp needs lo <= hi: a screen smaller than the popup puts it at the screen's corner.
  const int top = std::clamp(chip.top(), screen.top(), std::max(screen.top(), screen.bottom() - size.height() + 1));
  const int left =
      std::clamp(chip.left(), screen.left(), std::max(screen.left(), screen.right() - size.width() + 1));
  const std::array<QRect, 4> candidates{
      QRect(QPoint(chip.right() + 1 + kGap, top), size),             // right of the chip
      QRect(QPoint(chip.left() - kGap - size.width(), top), size),   // left of it
      QRect(QPoint(left, chip.bottom() + 1 + kGap), size),           // below
      QRect(QPoint(left, chip.top() - kGap - size.height()), size),  // above
  };
  for (const QRect& at : candidates)
    if (screen.contains(at) && !at.intersects(chip)) return at;
  // A chip as large as the screen: inside it, whatever it covers.
  return QRect(QPoint(left, top), size);
}

void GaugePopup::show_for(const QString& name, const QRect& global_chip) {
  gauge_ = name;
  update_contents();
  QScreen* screen = QGuiApplication::screenAt(global_chip.center());
  if (screen == nullptr) screen = QGuiApplication::primaryScreen();
  const QRect area = screen != nullptr ? screen->availableGeometry() : QRect(QPoint(0, 0), kSize * 4);
  setGeometry(placed(global_chip, kSize, area));
  show();
  raise();
}

void GaugePopup::refresh() {
  if (!isVisible()) return;
  update_contents();
}

void GaugePopup::update_contents() {
  const std::string name = gauge_.toStdString();
  // The latest reading, from the ten minutes before the series' newest: a
  // gauge that stopped reporting keeps its last value in the heading.
  const auto held = history_.series(name, Duration(kRange));
  QString text = gauge_;
  const auto latest = std::find_if(held.points.rbegin(), held.points.rend(),
                                   [](const TimeSeriesRing::Point& p) { return std::isfinite(p.value); });
  if (latest != held.points.rend())
    text += QStringLiteral("  %1 %2").arg(QString::number(latest->value, 'e', 2), held.units);
  heading_->setText(text);

  // The readings of the last ten minutes of the line's time, not of the
  // series: a gauge that stopped reporting is not drawn as if it had not.
  int shown = 0;
  ReadingHistory::Series recent;
  const TimePoint now = now_();
  const bool stale = held.newest && now - *held.newest > Duration(kRange);
  if (held.newest && !stale) {
    recent = history_.series(name, Duration(kRange) - (now - *held.newest));
    shown = static_cast<int>(std::ranges::count_if(
        recent.points, [](const TimeSeriesRing::Point& p) { return std::isfinite(p.value); }));
  }
  if (shown >= 2) {
    chart_->set_series(recent, kRange, wall_of_(*recent.newest));
    stack_->setCurrentWidget(chart_);
    message_->clear();
    return;
  }
  message_->setText(stale ? tr("No readings in the last 10 minutes") : tr("No readings yet"));
  stack_->setCurrentWidget(message_);
}

bool GaugePopup::eventFilter(QObject* watched, QEvent* event) {
  if (watched == parentWidget() && event->type() == QEvent::WindowDeactivate) hide();
  return QFrame::eventFilter(watched, event);
}

const QString& GaugePopup::gauge() const { return gauge_; }

QString GaugePopup::heading() const { return heading_->text(); }

QString GaugePopup::message() const { return stack_->currentWidget() == message_ ? message_->text() : QString(); }

HistoryChart* GaugePopup::chart() const { return chart_; }

}  // namespace pychron::ui
