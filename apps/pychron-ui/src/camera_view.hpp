#pragma once

// CameraView (laser window design, section 6): what a laser's camera sees,
// scaled to fit, with the aim point as a crosshair, the size a hole of the
// tray should be as a faint circle about it, and the target the finder sees
// outlined. A camera that fails keeps its last picture, greyed, under the
// reason.

#include <optional>

#include <QImage>
#include <QPointF>
#include <QString>
#include <QWidget>

#include "pychron/laser/laser_system.hpp"

namespace pychron::ui {

class CameraView : public QWidget {
  Q_OBJECT

 public:
  explicit CameraView(QWidget* parent = nullptr);

  void set_view(const laser::CameraView& seen);
  void set_failed(const QString& why);  // the last picture stays, greyed
  void clear(const QString& why);       // no picture: only the reason

  const QImage& image() const noexcept { return image_; }  // the frame, unscaled; null when there is none
  QString message() const { return message_; }
  bool has_target() const noexcept { return target_.has_value(); }
  // A frame pixel's place in the widget.
  QPointF to_widget(const QPointF& frame_px) const;

  QSize sizeHint() const override { return {300, 300}; }
  QSize minimumSizeHint() const override { return {160, 160}; }

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  struct Target {
    QPointF center;
    double radius = 0;
  };
  QRectF picture() const;  // where the frame is drawn

  QImage image_;
  QPointF aim_;
  double expected_radius_ = 0;
  std::optional<Target> target_;
  QString message_;
  bool stale_ = false;
};

}  // namespace pychron::ui
