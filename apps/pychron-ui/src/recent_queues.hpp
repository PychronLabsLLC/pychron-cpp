#pragma once

// The queues opened or saved last, newest first, for the experiment window's
// File > Open Recent (queue-directory design). Kept in the window's QSettings
// as a list of absolute paths; a path whose file is gone is left out and
// forgotten when the list is read.

#include <filesystem>

#include <QSettings>
#include <QStringList>

namespace pychron::ui {

class RecentQueues {
 public:
  static constexpr int kMax = 10;
  static constexpr const char* kKey = "experiment_window/recent_queues";

  // `settings` must outlive this object.
  explicit RecentQueues(QSettings& settings) : settings_(settings) {}

  // Puts `queue` (made absolute, its `..` and links resolved) first, drops
  // its older entry, and keeps the newest kMax.
  void add(const std::filesystem::path& queue);
  // The stored list without the paths that no longer exist; the stored list
  // is written back when one was dropped.
  [[nodiscard]] QStringList entries();
  void clear();

 private:
  QSettings& settings_;
};

}  // namespace pychron::ui
