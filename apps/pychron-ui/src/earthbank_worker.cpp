#include "earthbank_worker.hpp"

#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include <QThread>

#include "pychron/core/error.hpp"

namespace pychron::ui {

namespace eb = pychron::earthbank;

EarthbankSetup EarthbankSetup::system() {
  EarthbankSetup setup;
  setup.http = [] { return eb::make_http_client(); };
  setup.secrets = [] { return std::shared_ptr<eb::ISecretStore>(eb::make_secret_store()); };
  setup.profiles_file = eb::default_profiles_file();
  return setup;
}

QString earthbank_store_line(const eb::ISecretStore& store) {
  if (!store.persistent()) return QObject::tr("Passwords are not stored: this session only.");
  return QObject::tr("Passwords are kept in: %1.").arg(QString::fromStdString(store.backend()));
}

namespace {

// Where the worker's thread leaves its answers. The owner takes itself out
// when it goes away; an answer for an owner that is gone is dropped, and the
// thread, seeing `stopping`, drops what it has not begun.
struct Mailbox {
  std::mutex mutex;
  QObject* owner = nullptr;
  bool stopping = false;

  void post(std::function<void()> reply) {
    const std::scoped_lock lock(mutex);
    if (owner != nullptr) QMetaObject::invokeMethod(owner, std::move(reply), Qt::QueuedConnection);
  }
  bool is_stopping() {
    const std::scoped_lock lock(mutex);
    return stopping;
  }
};

QString profile_name(const eb::Profile& profile) { return QString::fromStdString(profile.name); }

}  // namespace

// On the worker's thread. The client's `send` may turn this thread's event
// loop while it waits (QtHttpClient does), and a request posted meanwhile
// would then start inside the one that is out: so what is posted is only put
// in a queue, and the queue is run from one place.
class EarthbankWorker::Job : public QObject {
 public:
  Job(std::function<std::unique_ptr<eb::IHttpClient>()> make_http, std::shared_ptr<eb::ISecretStore> secrets,
      std::shared_ptr<Mailbox> mailbox)
      : make_http_(std::move(make_http)), secrets_(std::move(secrets)), mailbox_(std::move(mailbox)) {}

  void enqueue(std::function<Reply(Job&)> work) {
    queue_.push_back(std::move(work));
    if (running_) return;
    running_ = true;
    while (!queue_.empty()) {
      auto next = std::move(queue_.front());
      queue_.pop_front();
      if (mailbox_->is_stopping()) continue;
      mailbox_->post(next(*this));
    }
    running_ = false;
    if (mailbox_->is_stopping()) thread()->quit();
  }

  eb::ISecretStore& secrets() { return *secrets_; }

  // A session of `profile`'s service, signed in with `typed`, or with the
  // stored password when nothing was typed. `typed_was_used` says which.
  Result<std::unique_ptr<eb::Session>> sign_in(const eb::Profile& profile, const std::string& typed) {
    if (!http_ && make_http_) http_ = make_http_();
    if (!http_) {
      return fail(ErrorKind::Config,
                  QObject::tr("This build cannot reach EarthBank: it was made without Qt Network.").toStdString());
    }
    if (const auto url = eb::checked_base_url(profile.base_url); !url) return Unexpected<Error>(url.error());
    std::string password = typed;
    if (password.empty()) {
      auto stored = secrets_->get(eb::secret_account(profile));
      if (!stored) return Unexpected<Error>(stored.error());
      if (!*stored) {
        return fail(ErrorKind::Config, QObject::tr("No password is kept for profile %1. Type it, then press Test.")
                                           .arg(profile_name(profile))
                                           .toStdString());
      }
      password = std::move(**stored);
    }
    auto session = std::make_unique<eb::Session>(*http_, profile.base_url);
    if (const auto in = session->sign_in(profile.username, password); !in) return Unexpected<Error>(in.error());
    return session;
  }

 private:
  std::function<std::unique_ptr<eb::IHttpClient>()> make_http_;
  std::unique_ptr<eb::IHttpClient> http_;
  std::shared_ptr<eb::ISecretStore> secrets_;
  std::shared_ptr<Mailbox> mailbox_;
  std::deque<std::function<Reply(Job&)>> queue_;
  bool running_ = false;
};

EarthbankWorker::EarthbankWorker(std::function<std::unique_ptr<eb::IHttpClient>()> http,
                                 std::shared_ptr<eb::ISecretStore> secrets, QObject* parent)
    : QObject(parent), thread_(new QThread) {
  auto mailbox = std::make_shared<Mailbox>();
  mailbox->owner = this;
  mailbox_ = mailbox;
  job_ = new Job(std::move(http), std::move(secrets), std::move(mailbox));
  job_->moveToThread(thread_);
  connect(thread_, &QThread::finished, job_, &QObject::deleteLater);
  thread_->setObjectName(QStringLiteral("earthbank"));
  thread_->start();
}

EarthbankWorker::~EarthbankWorker() {
  const auto mailbox = std::static_pointer_cast<Mailbox>(mailbox_);
  {
    const std::scoped_lock lock(mailbox->mutex);
    mailbox->owner = nullptr;
    mailbox->stopping = true;
  }
  // An empty request: the queue then sees `stopping` once the request that
  // is out, if any, has come back, and ends the thread from inside it.
  QMetaObject::invokeMethod(
      job_, [job = job_] { job->enqueue([](Job&) { return Reply([] {}); }); }, Qt::QueuedConnection);
  if (thread_->wait(250)) {
    delete thread_;
    return;
  }
  // Still waiting for the service. It is not waited for: the thread object
  // goes when the thread has ended.
  connect(thread_, &QThread::finished, thread_, &QObject::deleteLater);
  if (thread_->isFinished()) thread_->deleteLater();
}

void EarthbankWorker::ask(std::function<Reply(Job&)> work) {
  ++outstanding_;
  emit outstanding_changed(outstanding_);
  QMetaObject::invokeMethod(
      job_,
      [job = job_, this, work = std::move(work)]() mutable {
        job->enqueue([this, work = std::move(work)](Job& j) {
          Reply reply = work(j);
          return Reply([this, reply = std::move(reply)] {
            --outstanding_;
            reply();
            emit outstanding_changed(outstanding_);
          });
        });
      },
      Qt::QueuedConnection);
}

void EarthbankWorker::test(const eb::Profile& profile, const QString& password) {
  ask([this, profile, typed = password.toStdString()](Job& job) -> Reply {
    const QString name = profile_name(profile);
    auto session = job.sign_in(profile, typed);
    if (!session) return [this, name, what = QString::fromStdString(session.error().what)] { emit failed(name, what); };
    const auto account = (*session)->account();
    if (!account) return [this, name, what = QString::fromStdString(account.error().what)] { emit failed(name, what); };
    QString note;
    if (!typed.empty()) {
      if (const auto kept = job.secrets().set(eb::secret_account(profile), typed); !kept) {
        note = tr("The password was not kept: %1").arg(QString::fromStdString(kept.error().what));
      }
    }
    return [this, name, account = *account, note] {
      emit tested(name, QString::fromStdString(account.name), QString::fromStdString(account.login), note);
    };
  });
}

void EarthbankWorker::list_packages(const eb::Profile& profile, const QString& password) {
  ask([this, profile, typed = password.toStdString()](Job& job) -> Reply {
    const QString name = profile_name(profile);
    auto session = job.sign_in(profile, typed);
    if (!session) return [this, name, what = QString::fromStdString(session.error().what)] { emit failed(name, what); };
    auto packages = (*session)->packages(profile.institution_id);
    if (!packages) {
      return [this, name, what = QString::fromStdString(packages.error().what)] { emit failed(name, what); };
    }
    return [this, name, packages = std::move(*packages)] { emit packages_listed(name, packages); };
  });
}

void EarthbankWorker::create_package(const eb::Profile& profile, const QString& password, const QString& package) {
  ask([this, profile, typed = password.toStdString(), package = package.toStdString()](Job& job) -> Reply {
    const QString name = profile_name(profile);
    auto session = job.sign_in(profile, typed);
    if (!session) return [this, name, what = QString::fromStdString(session.error().what)] { emit failed(name, what); };
    auto made = (*session)->create_package(package, profile.institution_id);
    if (!made) return [this, name, what = QString::fromStdString(made.error().what)] { emit failed(name, what); };
    return [this, name, made = std::move(*made)] { emit package_created(name, made); };
  });
}

void EarthbankWorker::store_password(const eb::Profile& profile, const QString& password) {
  ask([this, profile, typed = password.toStdString()](Job& job) -> Reply {
    const auto kept = job.secrets().set(eb::secret_account(profile), typed);
    const QString problem = kept ? QString() : QString::fromStdString(kept.error().what);
    return [this, name = profile_name(profile), problem] { emit password_kept(name, problem); };
  });
}

void EarthbankWorker::erase_password(const eb::Profile& profile) {
  ask([this, profile](Job& job) -> Reply {
    const auto erased = job.secrets().erase(eb::secret_account(profile));
    const QString problem = erased ? QString() : QString::fromStdString(erased.error().what);
    return [this, name = profile_name(profile), problem] { emit password_kept(name, problem); };
  });
}

}  // namespace pychron::ui
