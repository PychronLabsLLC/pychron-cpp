#include "brand.hpp"

#include <array>
#include <cmath>

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QRadialGradient>
#include <QSysInfo>
#include <QTextDocumentFragment>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

#include "menu_hub.hpp"
#include "theme.hpp"

namespace pychron::ui {

namespace {

// Relative heights of the 36..40 peaks: an irradiated sample, so 39 stands
// well up, 40 dominates, and 37 and 38 are small.
constexpr std::array<double, 5> kPeakHeights{0.21, 0.12, 0.075, 0.47, 1.0};

QColor faded(QColor c, int alpha) {
  c.setAlpha(alpha);
  return c;
}

QFont brand_font(double pixel_size, QFont::Weight weight, double tracking = 0) {
  QFont f = QApplication::font();
  f.setPixelSize(std::max(1, static_cast<int>(std::lround(pixel_size))));
  f.setWeight(weight);
  if (tracking != 0) f.setLetterSpacing(QFont::AbsoluteSpacing, tracking);
  return f;
}

QString compiler() {
#if defined(__clang__)
  return QStringLiteral("clang %1.%2.%3").arg(__clang_major__).arg(__clang_minor__).arg(__clang_patchlevel__);
#elif defined(__GNUC__)
  return QStringLiteral("gcc %1.%2.%3").arg(__GNUC__).arg(__GNUC_MINOR__).arg(__GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
  return QStringLiteral("MSVC %1").arg(_MSC_FULL_VER);
#else
  return QStringLiteral("unknown");
#endif
}

}  // namespace

QString app_version() { return QStringLiteral(PYCHRON_VERSION); }

QString build_info() {
  QStringList lines;
  lines << QStringLiteral("pychron: %1").arg(app_version());
  lines << QStringLiteral("Qt: %1 (built with %2)").arg(QString::fromLatin1(qVersion()), QStringLiteral(QT_VERSION_STR));
  lines << QStringLiteral("compiler: %1").arg(compiler());
#ifdef NDEBUG
  lines << QStringLiteral("build: release");
#else
  lines << QStringLiteral("build: debug");
#endif
  lines << QStringLiteral("platform: %1 (%2, %3)")
               .arg(QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture(), QGuiApplication::platformName());
#if PYCHRON_SCRIPTING_ENABLED
  lines << QStringLiteral("scripting: embedded Python");
#else
  lines << QStringLiteral("scripting: off");
#endif
#ifdef PYCHRON_UI_HAS_STORE
  lines << QStringLiteral("DVC store: on");
#else
  lines << QStringLiteral("DVC store: off");
#endif
  return lines.join(QLatin1Char('\n'));
}

namespace brand {

void paint_peaks(QPainter& p, const QRectF& area, const QColor& color, bool labels) {
  p.save();
  p.setRenderHint(QPainter::Antialiasing);
  const double label_h = labels ? std::max(10.0, area.height() * 0.11) : 0.0;
  const double base = area.bottom() - label_h;
  const double height = base - area.top();
  const double step = area.width() / static_cast<double>(kPeakHeights.size());
  // Magnetic-sector peaks are flat-topped: a super-Gaussian, not a Gaussian.
  const double width = step * 0.2;
  const auto center = [&](std::size_t i) { return area.left() + step * (static_cast<double>(i) + 0.5); };
  const auto level = [&](double x) {
    double y = 0;
    for (std::size_t i = 0; i < kPeakHeights.size(); ++i) {
      const double u = (x - center(i)) / width;
      y += kPeakHeights[i] * std::exp(-u * u * u * u);
    }
    return base - y * height;
  };

  QPainterPath trace;
  const double dx = std::max(0.5, area.width() / 400.0);
  trace.moveTo(area.left(), level(area.left()));
  for (double x = area.left() + dx; x <= area.right(); x += dx) trace.lineTo(x, level(x));
  trace.lineTo(area.right(), level(area.right()));

  QPainterPath fill = trace;
  fill.lineTo(area.right(), base);
  fill.lineTo(area.left(), base);
  fill.closeSubpath();
  QLinearGradient glow(0, area.top(), 0, base);
  glow.setColorAt(0, faded(color, 120));
  glow.setColorAt(1, faded(color, 0));
  p.fillPath(fill, glow);

  p.setPen(QPen(color, std::max(1.2, height / 90.0), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  p.drawPath(trace);

  if (labels) {
    p.setPen(faded(color, 150));
    p.setFont(brand_font(label_h * 0.8, QFont::DemiBold, 0.5));
    for (std::size_t i = 0; i < kPeakHeights.size(); ++i) {
      const QRectF cell(center(i) - step / 2, base + label_h * 0.15, step, label_h);
      p.drawText(cell, Qt::AlignHCenter | Qt::AlignTop, QString::number(36 + static_cast<int>(i)));
    }
  }
  p.restore();
}

QPixmap banner(QSize size, qreal dpr, bool simulation, bool compact) {
  const Theme& t = theme();
  QPixmap pm(size * dpr);
  pm.setDevicePixelRatio(dpr);
  const double w = size.width();
  const double h = size.height();
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  p.setRenderHint(QPainter::TextAntialiasing);

  // Ink, a little lighter at the top left, and a glow behind the 40 peak.
  QLinearGradient bg(0, 0, w * 0.4, h);
  bg.setColorAt(0, t.chrome.lighter(135));
  bg.setColorAt(1, t.chrome);
  p.fillRect(QRectF(0, 0, w, h), bg);
  QRadialGradient halo(QPointF(w * 0.86, h * 0.42), h * 0.75);
  halo.setColorAt(0, faded(t.signal, 46));
  halo.setColorAt(1, faded(t.signal, 0));
  p.fillRect(QRectF(0, 0, w, h), halo);

  // Chart paper.
  p.setPen(QPen(faded(t.on_chrome, 12), 1));
  for (double y = 22; y < h; y += 22) p.drawLine(QPointF(0, y), QPointF(w, y));
  for (double x = 22; x < w; x += 44) p.drawLine(QPointF(x, 0), QPointF(x, h));

  const double margin = compact ? 24 : 32;
  const QRectF peaks(w * 0.5, h * (compact ? 0.12 : 0.14), w * 0.5 - margin, h * (compact ? 0.74 : 0.64));
  paint_peaks(p, peaks, t.signal, true);

  p.setPen(faded(t.on_chrome, 150));
  p.setFont(brand_font(compact ? 10 : 11, QFont::DemiBold, 2.2));
  p.drawText(QPointF(margin, margin + 6), QStringLiteral("PYCHRON LABS"));

  // Wordmark, a signal rule under it, then what it is.
  const double mark_px = h * (compact ? 0.24 : 0.17);
  const QFont mark = brand_font(mark_px, QFont::ExtraBold, -mark_px * 0.035);
  const double mark_base = h * (compact ? 0.56 : 0.5);
  p.setFont(mark);
  p.setPen(t.on_chrome);
  p.drawText(QPointF(margin - mark_px * 0.04, mark_base), QStringLiteral("pychron"));
  const double rule_y = mark_base + mark_px * 0.22;
  p.fillRect(QRectF(margin, rule_y, mark_px * 0.9, std::max(2.0, mark_px * 0.06)), t.signal);

  const double tag_px = compact ? 12 : 13.5;
  p.setFont(brand_font(tag_px, QFont::Medium));
  p.setPen(faded(t.on_chrome, 215));
  const double tag_y = rule_y + tag_px * 2.1;
  p.drawText(QPointF(margin, tag_y), QStringLiteral("Noble-gas geochronology"));
  if (!compact) {
    p.setPen(faded(t.on_chrome, 140));
    p.drawText(QPointF(margin, tag_y + tag_px * 1.45), QStringLiteral("acquisition · control · reduction"));
  }
  p.setFont(brand_font(compact ? 11 : 12, QFont::DemiBold, 0.4));
  p.setPen(t.signal);
  p.drawText(QPointF(margin, tag_y + tag_px * (compact ? 1.6 : 3.1)), QStringLiteral("version ") + app_version());

  if (simulation) {
    const QFont badge = brand_font(10.5, QFont::Bold, 1.6);
    const QString label = QStringLiteral("SIMULATION");
    const double bw = QFontMetricsF(badge).horizontalAdvance(label) + 22;
    const QRectF pill(w - margin - bw, margin - 9, bw, 22);
    p.setPen(Qt::NoPen);
    p.setBrush(t.warning);
    p.drawRoundedRect(pill, 11, 11);
    p.setFont(badge);
    p.setPen(t.chrome);
    p.drawText(pill, Qt::AlignCenter, label);
  }

  // A signal line along the foot, fading out to the right.
  QLinearGradient foot(0, 0, w, 0);
  foot.setColorAt(0, t.signal);
  foot.setColorAt(1, faded(t.signal, 0));
  p.fillRect(QRectF(0, h - 3, w, 3), foot);
  return pm;
}

QPixmap icon_pixmap(int s) {
  const Theme& t = theme();
  QPixmap pm(s, s);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  const QRectF tile(0, 0, s, s);
  QLinearGradient bg(0, 0, 0, s);
  bg.setColorAt(0, t.chrome.lighter(150));
  bg.setColorAt(1, t.chrome);
  p.setPen(Qt::NoPen);
  p.setBrush(bg);
  p.drawRoundedRect(tile, s * 0.22, s * 0.22);
  const double m = s * 0.14;
  if (s >= 48) {
    paint_peaks(p, QRectF(m * 0.6, m * 1.3, s - m * 1.2, s - m * 2.4), t.signal, false);
    p.fillRect(QRectF(m, s - m * 1.1, s - 2 * m, std::max(1.0, s / 40.0)), t.signal);
  } else {
    // Too small for the trace: 36, 39 and 40 as bars on a baseline.
    const double base = s - m * 1.2;
    const double top = m * 1.2;
    const double bar = (s - 2 * m) / 5.0;
    const std::array<std::pair<double, double>, 3> bars{{{0, 0.3}, {2, 0.6}, {4, 1.0}}};
    p.setBrush(t.signal);
    for (const auto& [slot, rel] : bars) {
      const double bh = (base - top) * rel;
      p.drawRoundedRect(QRectF(m + slot * bar, base - bh, bar, bh), bar * 0.3, bar * 0.3);
    }
  }
  p.end();
  return pm;
}

QIcon app_icon() {
  QIcon icon;
  for (const int s : {16, 24, 32, 48, 64, 128, 256}) icon.addPixmap(icon_pixmap(s));
  return icon;
}

QAction* add_help_menu(QMainWindow* window) {
  window->setWindowIcon(app_icon());
  auto* about = new QAction(QStringLiteral("About pychron"), window);
  about->setMenuRole(QAction::AboutRole);
  MenuHub::instance().contribute(window, MenuHub::Menu::Help, {about}, MenuHub::Scope::App);
  QObject::connect(about, &QAction::triggered, window, [window] {
    auto* dialog = window->findChild<AboutDialog*>(QString(), Qt::FindDirectChildrenOnly);
    if (dialog == nullptr) dialog = new AboutDialog(window);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
  });
  return about;
}

}  // namespace brand

namespace {

constexpr QSize kSplashSize{600, 340};

}  // namespace

SplashScreen::SplashScreen(bool simulation)
    : QSplashScreen(brand::banner(kSplashSize, qApp->devicePixelRatio(), simulation, false)),
      simulation_(simulation) {
  setWindowIcon(brand::app_icon());
}

void SplashScreen::showEvent(QShowEvent* event) {
  QSplashScreen::showEvent(event);
  // Up for the minimum from when it could be seen: painting the banner (its
  // fonts, the first time) can itself take a good part of the minimum.
  if (!shown_.isValid()) shown_.start();
}

void SplashScreen::finish_after(QWidget* window, std::chrono::milliseconds minimum) {
  raise();  // over the window just shown, where no window manager keeps it there
  const auto up = std::chrono::milliseconds(shown_.isValid() ? shown_.elapsed() : 0);
  const auto left = minimum - up;
  if (left <= std::chrono::milliseconds::zero()) {
    finish(window);
    return;
  }
  QTimer::singleShot(left, this, [this, target = QPointer<QWidget>(window)] {
    if (target) {
      finish(target);
    } else {
      close();
    }
  });
}

void SplashScreen::status(const QString& text) {
  showMessage(text);  // repaints now
  QApplication::processEvents();
}

void SplashScreen::drawContents(QPainter* painter) {
  const QString text = message();
  if (text.isEmpty()) return;
  const QRectF line(32, kSplashSize.height() - 34, kSplashSize.width() - 64, 18);
  const QFont font = brand_font(12, QFont::Medium);
  painter->setFont(font);
  painter->setPen(faded(theme().on_chrome, 190));
  painter->drawText(line, Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetricsF(font).elidedText(text, Qt::ElideRight, line.width()));
}

AboutDialog::AboutDialog(QWidget* parent) : QDialog(parent) {
  const Theme& t = theme();
  setWindowTitle(QStringLiteral("About pychron"));
  setWindowIcon(brand::app_icon());

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  const QSize banner_size(560, 190);
  banner_ = new QLabel;
  banner_->setPixmap(brand::banner(banner_size, devicePixelRatioF(), false, true));
  banner_->setFixedSize(banner_size);
  layout->addWidget(banner_);

  auto* content = new QWidget;
  auto* column = new QVBoxLayout(content);
  column->setContentsMargins(24, 18, 24, 18);
  column->setSpacing(12);
  layout->addWidget(content);

  QString components = QStringLiteral("Qt, QCustomPlot, toml++, spdlog and asio");
#if PYCHRON_SCRIPTING_ENABLED
  components = QStringLiteral("Qt, QCustomPlot, toml++, spdlog, asio, pybind11 and CPython");
#endif
#ifdef PYCHRON_UI_HAS_STORE
  components += QStringLiteral("; the DVC store on TinyORM");
#endif
  body_ = new QLabel(
      QStringLiteral(
          "<p style='margin-top:0'>Data acquisition and control for noble-gas (Ar/Ar) geochronology labs: "
          "extraction lines, mass spectrometers, automated experiment queues and data reduction. "
          "A C\u2060+\u2060+ rewrite of <a href='https://github.com/PychronLabsLLC/pychron'>pychron</a>.</p>"
          "<p><span style='color:%1'><b>In development.</b></span> No driver has yet been run against a real "
          "instrument; everything runs against the built-in simulator.</p>"
          "<p style='color:%2'>Free software under the GNU General Public License v3, with no warranty. "
          "Source: <a href='https://github.com/PychronLabsLLC/pychron-cpp'>PychronLabsLLC/pychron-cpp</a>.<br>"
          "Built with %3.</p>")
          .arg(t.warning_text.name(), t.muted_text.name(), components));
  body_->setWordWrap(true);
  body_->setTextFormat(Qt::RichText);
  body_->setOpenExternalLinks(true);
  body_->setMaximumWidth(banner_size.width() - 48);
  column->addWidget(body_);

  build_ = new QLabel(build_info());
  build_->setFont(style::mono_font());
  build_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  build_->setStyleSheet(QStringLiteral("QLabel { background: %1; border: 1px solid %2; border-radius: 6px; padding: 8px 10px; }")
                            .arg(t.alt_base.name(), t.border.name()));
  style::set_tone(build_, style::Tone::Muted);
  column->addWidget(build_);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
  copy_ = buttons->addButton(QStringLiteral("Copy build info"), QDialogButtonBox::ActionRole);
  copy_->setAutoDefault(false);  // Close stays the default
  connect(copy_, &QPushButton::clicked, this, [this] {
    QApplication::clipboard()->setText(build_info());
    copy_->setText(QStringLiteral("Copied"));
  });
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  buttons->button(QDialogButtonBox::Close)->setDefault(true);
  column->addWidget(buttons);

  layout->setSizeConstraint(QLayout::SetFixedSize);
}

QString AboutDialog::text() const {
  return QTextDocumentFragment::fromHtml(body_->text()).toPlainText() + QLatin1Char('\n') + build_->text();
}

}  // namespace pychron::ui
