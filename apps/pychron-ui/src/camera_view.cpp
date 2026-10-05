#include "camera_view.hpp"

#include <algorithm>

#include <QPainter>

#include "theme.hpp"

namespace pychron::ui {

namespace {

QImage to_image(const vision::Frame& frame) {
  if (frame.width <= 0 || frame.height <= 0 || frame.data.size() < static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height)) {
    return {};
  }
  QImage image(frame.width, frame.height, QImage::Format_Grayscale8);
  const double depth = frame.pixel_depth > 0 ? frame.pixel_depth : 255;
  for (int y = 0; y < frame.height; ++y) {
    uchar* row = image.scanLine(y);
    const std::uint16_t* from = frame.data.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width);
    if (frame.pixel_depth == 255) {
      // the usual camera: a byte a pixel already (this runs for every frame of video)
      for (int x = 0; x < frame.width; ++x) row[x] = static_cast<uchar>(from[x] > 255 ? 255 : from[x]);
    } else {
      for (int x = 0; x < frame.width; ++x) {
        row[x] = static_cast<uchar>(std::clamp(from[x] * 255.0 / depth, 0.0, 255.0));
      }
    }
  }
  return image;
}

}  // namespace

CameraView::CameraView(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("camera"));
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  message_ = tr("no picture yet");
}

void CameraView::set_view(const laser::CameraView& seen) {
  image_ = to_image(seen.frame);
  aim_ = QPointF(seen.aim_px.x, seen.aim_px.y);
  expected_radius_ = seen.expected_radius_px;
  target_.reset();
  if (seen.target) target_ = Target{QPointF(seen.target->center_px.x, seen.target->center_px.y), seen.target->radius_px};
  message_.clear();
  stale_ = false;
  // A live camera that has stopped: this is its last picture, not now's.
  if (!seen.trouble.empty()) {
    target_.reset();
    stale_ = true;
    message_ = tr("%1\nlast picture %2 s ago").arg(QString::fromStdString(seen.trouble)).arg(seen.age_ms / 1000);
  }
  update();
}

void CameraView::set_failed(const QString& why) {
  message_ = why;
  stale_ = true;
  target_.reset();
  update();
}

void CameraView::clear(const QString& why) {
  image_ = QImage();
  target_.reset();
  message_ = why;
  stale_ = false;
  update();
}

QRectF CameraView::picture() const {
  if (image_.isNull()) return {};
  const double scale = std::min(width() / static_cast<double>(image_.width()), height() / static_cast<double>(image_.height()));
  const double w = image_.width() * scale, h = image_.height() * scale;
  return {(width() - w) / 2, (height() - h) / 2, w, h};
}

QPointF CameraView::to_widget(const QPointF& frame_px) const {
  const QRectF box = picture();
  if (box.isEmpty()) return {};
  const double scale = box.width() / image_.width();
  // a pixel's center is at its integer coordinate
  return {box.left() + (frame_px.x() + 0.5) * scale, box.top() + (frame_px.y() + 0.5) * scale};
}

void CameraView::paintEvent(QPaintEvent*) {
  const Theme& t = theme();
  QPainter p(this);
  p.fillRect(rect(), t.chrome);
  if (image_.isNull()) {
    p.setPen(t.on_chrome);
    p.drawText(rect().adjusted(8, 8, -8, -8), Qt::AlignCenter | Qt::TextWordWrap, message_);
    return;
  }
  const QRectF box = picture();
  p.setRenderHint(QPainter::SmoothPixmapTransform, false);
  p.drawImage(box, image_);
  if (stale_) {
    QColor veil = t.chrome;
    veil.setAlpha(170);
    p.fillRect(box, veil);
    p.setPen(t.on_chrome);
    p.drawText(box.adjusted(8, 8, -8, -8), Qt::AlignCenter | Qt::TextWordWrap, message_);
    return;
  }
  const double scale = box.width() / image_.width();
  const QPointF aim = to_widget(aim_);
  p.setRenderHint(QPainter::Antialiasing, true);
  if (expected_radius_ > 0) {
    QPen faint(t.signal, 1, Qt::DashLine);
    p.setPen(faint);
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(aim, expected_radius_ * scale, expected_radius_ * scale);
  }
  if (target_) {
    p.setPen(QPen(t.ok, 2));
    p.setBrush(Qt::NoBrush);
    const double r = std::max(target_->radius * scale, 3.0);
    p.drawEllipse(to_widget(target_->center), r, r);
  }
  p.setRenderHint(QPainter::Antialiasing, false);
  p.setPen(QPen(t.error, 1));
  p.drawLine(QPointF(box.left(), aim.y()), QPointF(box.right(), aim.y()));
  p.drawLine(QPointF(aim.x(), box.top()), QPointF(aim.x(), box.bottom()));
}

}  // namespace pychron::ui
