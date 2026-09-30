#pragma once

// HealthBar: one chip per transport, green (connected, no errors), amber
// (connected with errors), red (down), grey (no report yet), plus the age of
// the last healthy report, refreshed once a second.

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QWidget>

#include "pychron/core/events.hpp"

namespace pychron::ui {

class HealthBar : public QWidget {
  Q_OBJECT

 public:
  enum class Status { Unknown, Ok, Degraded, Down };

  // `now` must use the core Clock's epoch; defaults to steady_clock.
  explicit HealthBar(QWidget* parent = nullptr, std::function<TimePoint()> now = {});

  // Adds a grey chip for each transport not shown yet.
  void seed(const std::vector<std::string>& transports);
  void update_health(const TransportHealth& health);
  // Re-renders every chip's age text.
  void refresh();

  static Status status_of(const TransportHealth& health);
  static QColor color_of(Status status);

  std::optional<Status> status(const std::string& transport) const;
  QLabel* chip(const std::string& transport) const;

 private:
  struct Chip {
    QLabel* label = nullptr;
    Status status = Status::Unknown;
    std::optional<TimePoint> last_ok;
  };

  Chip& chip_for(const std::string& transport);
  void render(const std::string& name, const Chip& chip);

  std::function<TimePoint()> now_;
  QHBoxLayout* layout_;
  QTimer ticker_;
  std::map<std::string, Chip> chips_;
};

}  // namespace pychron::ui
