#include "earthbank_worker.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <map>
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
  std::atomic<bool> cancelled{false};  // EarthbankWorker::cancel

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

QString qs(const std::string& text) { return QString::fromStdString(text); }

constexpr std::array<eb::AgeKind, 4> kAgeKinds = {eb::AgeKind::Plateau, eb::AgeKind::WeightedMean,
                                                  eb::AgeKind::Isochron, eb::AgeKind::Integrated};

// One sample of a publication: its analyses, and the options with what the
// lab said of it and of no other sample.
EarthbankSampleRow survey_sample(const std::string& sample, const std::vector<processing::AnalysisPtr>& analyses,
                                 eb::PublishOptions options) {
  EarthbankSampleRow row;
  row.sample = qs(sample);
  row.analyses = static_cast<int>(analyses.size());
  eb::SampleOptions said;
  if (const auto it = options.samples.find(sample); it != options.samples.end()) said = it->second;
  auto build = [&](std::optional<eb::AgeKind> kind) {
    said.preferred_age = kind;
    options.samples = {{sample, said}};
    return eb::build_submissions(analyses, options);
  };
  const auto ruled = build(std::nullopt);
  if (ruled && !ruled->empty()) {
    const eb::Submission& s = ruled->front();
    for (const eb::AgeKind kind : kAgeKinds) {
      if (s.age_summary.ageCalcTypeName == std::string(eb::age_type_name(kind))) row.rule = kind;
    }
    if (s.age_calc.fluxMonitorName) row.flux_monitor = qs(*s.age_calc.fluxMonitorName);
    for (const std::string& note : s.notes) row.notes << qs(note);
  } else if (!ruled) {
    row.rule_says = qs(ruled.error().what);
  }
  bool any = false;
  for (const eb::AgeKind kind : kAgeKinds) {
    EarthbankCandidate candidate;
    candidate.kind = kind;
    if (const auto built = build(kind); built && !built->empty()) {
      const eb::Submission& s = built->front();
      candidate.age = s.age_summary.age;
      candidate.uncertainty = s.age_summary.ageUncertainty;
      if (row.flux_monitor.isEmpty() && s.age_calc.fluxMonitorName) row.flux_monitor = qs(*s.age_calc.fluxMonitorName);
      any = true;
    } else if (!built) {
      candidate.unavailable = qs(built.error().what);
    }
    row.candidates.push_back(std::move(candidate));
  }
  if (!any) row.refused = row.rule_says.isEmpty() ? QObject::tr("No age can be computed for it.") : row.rule_says;
  return row;
}

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
  // Something to say before the request is done (progress).
  void post(Reply reply) { mailbox_->post(std::move(reply)); }
  bool should_stop() { return mailbox_->cancelled.load() || mailbox_->is_stopping(); }

  // An upload that has been shown to the user and not yet sent.
  struct Prepared {
    std::unique_ptr<eb::Session> session;
    std::unique_ptr<eb::LookupResolver> resolver;
    std::vector<eb::Submission> submissions;
    eb::UploadOptions options;
  };

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

 public:
  // After http_: its session talks through it and goes first.
  std::unique_ptr<Prepared> prepared;

 private:
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

void EarthbankWorker::cancel() { std::static_pointer_cast<Mailbox>(mailbox_)->cancelled.store(true); }

void EarthbankWorker::survey(const EarthbankPublication& publication) {
  std::static_pointer_cast<Mailbox>(mailbox_)->cancelled.store(false);
  ask([this, publication](Job& job) -> Reply {
    std::map<std::string, std::vector<processing::AnalysisPtr>> by_sample;
    for (const auto& analysis : publication.analyses) by_sample[analysis->sample].push_back(analysis);
    std::vector<EarthbankSampleRow> rows;
    const int total = static_cast<int>(by_sample.size());
    for (const auto& [sample, analyses] : by_sample) {
      if (job.should_stop()) break;
      rows.push_back(survey_sample(sample, analyses, publication.options));
      const int done = static_cast<int>(rows.size());
      job.post([this, done, total, name = rows.back().sample] { emit progress(done, total, name); });
    }
    return [this, rows = std::move(rows)] { emit surveyed(rows); };
  });
}

void EarthbankWorker::preflight(const eb::Profile& profile, const QString& password,
                                const EarthbankPublication& publication) {
  ask([this, profile, typed = password.toStdString(), publication](Job& job) -> Reply {
    const QString name = profile_name(profile);
    auto failure = [this, name](const Error& error) {
      return Reply([this, name, what = qs(error.what)] { emit failed(name, what); });
    };
    const auto submissions = eb::build_submissions(publication.analyses, publication.options);
    if (!submissions) return failure(submissions.error());
    auto session = job.sign_in(profile, typed);
    if (!session) return failure(session.error());
    eb::LookupResolver resolver(**session);
    return [this, report = eb::preflight(*submissions, resolver)] { emit preflighted(report); };
  });
}

void EarthbankWorker::export_xlsx(const EarthbankPublication& publication, const std::filesystem::path& folder,
                                  const eb::XlsxOptions& options) {
  ask([this, publication, folder, options](Job&) -> Reply {
    auto failure = [this](const Error& error) {
      return Reply([this, what = qs(error.what)] { emit failed(QString(), what); });
    };
    const auto submissions = eb::build_submissions(publication.analyses, publication.options);
    if (!submissions) return failure(submissions.error());
    auto written = eb::export_xlsx(*submissions, folder, options);
    if (!written) return failure(written.error());
    return [this, written = std::move(*written)] { emit exported(written); };
  });
}

void EarthbankWorker::prepare_upload(const eb::Profile& profile, const QString& password,
                                     const EarthbankPublication& publication, const EarthbankUploadSettings& settings) {
  std::static_pointer_cast<Mailbox>(mailbox_)->cancelled.store(false);
  ask([this, profile, typed = password.toStdString(), publication, settings](Job& job) -> Reply {
    job.prepared.reset();
    const QString name = profile_name(profile);
    auto failure = [this, name](const QString& what) {
      return Reply([this, name, what] { emit failed(name, tr("%1 Nothing was sent.").arg(what)); });
    };
    const std::optional<std::int64_t> package = settings.package ? settings.package : profile.data_package_id;
    if (!package) return failure(qs(eb::Session::no_package_sentence()));
    if (settings.journal_dir.empty()) return failure(tr("There is no folder to keep the upload's journal in."));
    auto submissions = eb::build_submissions(publication.analyses, publication.options);
    if (!submissions) return failure(qs(submissions.error().what));
    auto session = job.sign_in(profile, typed);
    if (!session) return failure(qs(session.error().what));

    // Who and where: asked of the service, so that what is confirmed is what
    // it holds. An upload that cannot say either does not start.
    EarthbankConfirmation confirmation;
    confirmation.profile = name;
    confirmation.base_url = qs((*session)->base_url());
    const auto account = (*session)->account();
    if (!account) return failure(qs(account.error().what));
    confirmation.account = *account;
    const auto packages = (*session)->packages(std::nullopt);
    if (!packages) return failure(qs(packages.error().what));
    const auto into = std::find_if(packages->begin(), packages->end(),
                                   [&](const eb::DataPackage& p) { return p.id == *package; });
    if (into == packages->end()) {
      return failure(tr("Data package %1 is not among the packages EarthBank lists.").arg(*package));
    }
    confirmation.package = *into;
    for (const eb::Submission& s : *submissions) {
      confirmation.samples.push_back({qs(s.sample.name.value_or(s.datapoint_name)), s.aliquots.size(), s.measurements.size()});
    }

    auto prepared = std::make_unique<Job::Prepared>();
    prepared->resolver = std::make_unique<eb::LookupResolver>(**session);
    confirmation.preflight = eb::preflight(*submissions, *prepared->resolver);
    const std::size_t unasked = confirmation.preflight.could_not_ask().size();
    if (unasked > 0) {
      confirmation.blocked = tr("Nothing was sent: %n term(s) could not be asked about, and EarthBank may know them. "
                                "Try again.", nullptr, static_cast<int>(unasked));
    } else if (!confirmation.preflight.clean() && !settings.allow_unresolved) {
      confirmation.blocked = tr("Nothing was sent: correct the terms EarthBank does not know, or tick Allow unresolved "
                                "terms to send without those fields.");
    }
    if (confirmation.blocked.isEmpty()) {
      prepared->session = std::move(*session);
      prepared->submissions = std::move(*submissions);
      prepared->options.data_package_id = *package;
      prepared->options.allow_unresolved = settings.allow_unresolved;
      prepared->options.new_datapoint = settings.new_datapoint;
      prepared->options.resolve_in_doubt = settings.resolve_in_doubt;
      prepared->options.in_doubt_setting = tr("the in-doubt answer (below)").toStdString();
      prepared->options.new_datapoint_setting = tr("\"Send as new records\" ticked").toStdString();
      prepared->options.journal_dir = settings.journal_dir;
      job.prepared = std::move(prepared);
    }
    return [this, confirmation = std::move(confirmation)] { emit upload_prepared(confirmation); };
  });
}

void EarthbankWorker::upload() {
  ask([this](Job& job) -> Reply {
    const std::unique_ptr<Job::Prepared> prepared = std::move(job.prepared);
    if (!prepared) return [this] { emit failed(QString(), tr("There is no upload to send. Nothing was sent.")); };
    eb::UploadOptions options = prepared->options;
    options.on_event = [this, &job](const eb::UploadEvent& event) {
      if (event.record.empty()) return;
      const QString line = qs(event.sample + ": " + event.record + ": " + event.detail);
      job.post([this, done = static_cast<int>(event.done), total = static_cast<int>(event.total), line] {
        emit progress(done, total, line);
      });
    };
    options.cancelled = [&job] { return job.should_stop(); };
    auto report = eb::upload(prepared->submissions, *prepared->session, *prepared->resolver, options);
    if (!report) return [this, what = qs(report.error().what)] { emit failed(QString(), what); };
    return [this, report = std::move(*report)] { emit uploaded(report); };
  });
}

void EarthbankWorker::abandon_upload() {
  ask([](Job& job) -> Reply {
    job.prepared.reset();
    return [] {};
  });
}

}  // namespace pychron::ui
