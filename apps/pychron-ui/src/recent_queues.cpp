#include "recent_queues.hpp"

#include <system_error>

namespace pychron::ui {

namespace fs = std::filesystem;

void RecentQueues::add(const fs::path& queue) {
  std::error_code ec;
  fs::path absolute = fs::absolute(queue, ec);
  if (ec) absolute = queue;
  fs::path canonical = fs::weakly_canonical(absolute, ec);
  if (ec) canonical = absolute.lexically_normal();
  const QString entry = QString::fromStdU16String(canonical.u16string());  // not string(): ANSI on Windows

  QStringList list = settings_.value(QLatin1String(kKey)).toStringList();
  list.removeAll(entry);
  list.prepend(entry);
  while (list.size() > kMax) list.removeLast();
  settings_.setValue(QLatin1String(kKey), list);
}

QStringList RecentQueues::entries() {
  const QStringList stored = settings_.value(QLatin1String(kKey)).toStringList();
  QStringList kept;
  for (const QString& entry : stored) {
    std::error_code ec;
    if (fs::is_regular_file(fs::path(entry.toStdU16String()), ec)) kept.append(entry);
  }
  if (kept.size() != stored.size()) settings_.setValue(QLatin1String(kKey), kept);
  return kept;
}

void RecentQueues::clear() { settings_.remove(QLatin1String(kKey)); }

}  // namespace pychron::ui
