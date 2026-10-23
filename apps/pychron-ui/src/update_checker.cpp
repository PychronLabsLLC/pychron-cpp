#include "update_checker.hpp"

#include <utility>

#include <QDateTime>
#include <QMetaObject>
#include <QSettings>

#include "pychron/core/log_context.hpp"

namespace pychron::ui {

namespace {

const QString kEnabledKey = QStringLiteral("updates/check_on_startup");
const QString kLastCheckKey = QStringLiteral("updates/last_check");
const QString kSkippedKey = QStringLiteral("updates/skipped_version");

// A failure the network had (unreachable, slow, a status GitHub chose) is
// information; an answer that does not read is something to look into.
bool is_warning(const Error& error) {
  return error.kind == ErrorKind::Protocol && !error.what.starts_with("GitHub answered ");
}

// What the operator is told (spec section 4).
QString failure_text(const Error& error) {
  QString what = QString::fromStdString(error.what);
  if (error.kind == ErrorKind::Io || error.kind == ErrorKind::Timeout) {
    return QStringLiteral("Could not reach GitHub (%1)").arg(what);
  }
  if (is_warning(error)) return QStringLiteral("Unexpected answer from GitHub (%1)").arg(what);
  return what;
}

}  // namespace

update::CheckPolicy load_update_policy(const QSettings& settings) {
  update::CheckPolicy policy;
  policy.enabled = settings.value(kEnabledKey, true).toBool();
  const QDateTime last = QDateTime::fromString(settings.value(kLastCheckKey).toString(), Qt::ISODate);
  if (last.isValid()) {
    policy.last_check = std::chrono::system_clock::time_point(std::chrono::milliseconds(last.toMSecsSinceEpoch()));
  }
  if (const auto skipped = update::parse_version(settings.value(kSkippedKey).toString().toStdString())) {
    policy.skipped = *skipped;
  }
  return policy;
}

void save_update_policy(QSettings& settings, const update::CheckPolicy& policy) {
  settings.setValue(kEnabledKey, policy.enabled);
  if (policy.last_check) {
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(policy.last_check->time_since_epoch()).count();
    settings.setValue(kLastCheckKey, QDateTime::fromMSecsSinceEpoch(ms).toUTC().toString(Qt::ISODate));
  } else {
    settings.remove(kLastCheckKey);
  }
  if (policy.skipped) {
    settings.setValue(kSkippedKey, QString::fromStdString(update::to_string(*policy.skipped)));
  } else {
    settings.remove(kSkippedKey);
  }
}

UpdateChecker::UpdateChecker(QSettings& settings, update::Version running, Now now, ClientFactory make_client,
                             QObject* parent)
    : QObject(parent),
      settings_(settings),
      running_(running),
      now_(std::move(now)),
      make_client_(std::move(make_client)) {}

// Joined here, before ~QObject: an answer the worker posted meanwhile is then
// removed with this object's other pending events and never runs.
UpdateChecker::~UpdateChecker() {
  if (worker_.joinable()) worker_.join();
}

void UpdateChecker::set_log(std::function<void(const QString& line)> log) { log_ = std::move(log); }

void UpdateChecker::check_on_startup() {
  if (busy_ || !update::due(load_update_policy(settings_), now_())) return;
  auto client = make_client_();
  if (!client) return;
  start(false, std::move(client));
}

void UpdateChecker::check_now() {
  if (busy_) return;
  auto client = make_client_();
  if (!client) {
    emit failed(QStringLiteral("this build has no network client"), true);
    return;
  }
  start(true, std::move(client));
}

void UpdateChecker::skip(update::Version version) {
  auto policy = load_update_policy(settings_);
  policy.skipped = version;
  save_update_policy(settings_, policy);
}

void UpdateChecker::start(bool manual, std::unique_ptr<http::IHttpClient> client) {
  if (worker_.joinable()) worker_.join();  // the last one has posted its answer and is ending
  set_busy(true);
  worker_ = std::thread([this, manual, client = std::move(client)] {
    const LogContext::Thread log_thread("update");
    auto result = update::fetch_latest(*client);
    QMetaObject::invokeMethod(
        this, [this, manual, answer = std::move(result)] { finish(manual, answer); }, Qt::QueuedConnection);
  });
}

void UpdateChecker::set_busy(bool busy) {
  if (busy_ == busy) return;
  busy_ = busy;
  emit busy_changed(busy);
}

void UpdateChecker::finish(bool manual, const Result<update::Fetched>& result) {
  set_busy(false);
  if (!result) {
    const Error& error = result.error();
    if (!manual && log_) {
      log_((is_warning(error) ? QStringLiteral("WARN [update] ") : QStringLiteral("INFO [update] ")) +
           QStringLiteral("could not check for a newer release: ") + QString::fromStdString(error.what));
    }
    emit failed(failure_text(error), manual);
    return;
  }

  auto policy = load_update_policy(settings_);
  policy.last_check = now_();
  save_update_policy(settings_, policy);

  if (result->outcome == update::FetchOutcome::NoRelease || !result->release) {
    if (manual) emit up_to_date(true, std::nullopt);
    return;
  }
  const update::Release& release = *result->release;
  if (manual) {
    if (release.version > running_) {
      emit available(release, true);
    } else {
      emit up_to_date(false, release.version);
    }
    return;
  }
  if (const auto shown = update::worth_showing(release, running_, policy)) emit available(*shown, false);
}

}  // namespace pychron::ui
