#pragma once

// HealthBar: one chip per transport, green (connected, no errors), amber
// (connected with errors), red (down), grey (no report yet), plus the age of
// the last healthy report, refreshed once a second. While the queue holds
// (spec 4.6), a chip ahead of them says why and counts down to the hold
// limit, at which the queue ends: "holding: <reason> · ends in 1:59:30".

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

#include "theme.hpp"

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
  // Re-renders every chip's age text and the hold's countdown.
  void refresh();

  // The queue holds for `reason` until `until` (ExecutorHolding::until, on
  // the core Clock, as `now`); clear_hold() when it no longer does.
  void show_hold(const QString& reason, TimePoint until);
  void clear_hold();
  bool holding() const noexcept { return hold_.has_value(); }
  // The hold chip's text at `now`; empty without a hold.
  QString hold_text_at(TimePoint now) const;
  // As shown (empty without a hold).
  QString hold_text() const;

  static Status status_of(const TransportHealth& health);
  static style::Level level_of(Status status);

  std::optional<Status> status(const std::string& transport) const;
  QLabel* chip(const std::string& transport) const;

 private:
  struct Chip {
    QLabel* label = nullptr;
    Status status = Status::Unknown;
    std::optional<TimePoint> last_ok;
  };

  struct Hold {
    QString reason;
    TimePoint until;
  };

  Chip& chip_for(const std::string& transport);
  void render(const std::string& name, const Chip& chip);
  void render_hold();

  std::function<TimePoint()> now_;
  QHBoxLayout* layout_;
  QLabel* hold_chip_;
  std::optional<Hold> hold_;
  QTimer ticker_;
  std::map<std::string, Chip> chips_;
};

}  // namespace pychron::ui
