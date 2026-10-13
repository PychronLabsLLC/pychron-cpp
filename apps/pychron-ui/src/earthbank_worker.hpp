#pragma once

// EarthBank from the UI: everything that talks to the service, or to the
// system's password store, runs here on a thread of its own, so that a slow
// answer (or a keychain asking to be unlocked) never stops the window from
// painting.
//
// A request is asked for from the UI's thread and returns at once; its answer
// comes back there as a signal. Requests are queued and run one at a time, in
// the order asked. Each signs in for itself and keeps nothing between
// requests: no token outlives the request it was got for.
//
// The password a request is given is the one just typed; empty, the one the
// store has for the profile. A password that is typed and signs in is put in
// the store by `test`. No password or token is in any signal: what went wrong
// is the service library's own sentence (session.hpp), which has both taken
// out.
//
// Publishing (the data browser's Publish to EarthBank) runs here too: the
// survey of the selected analyses (what each sample would report, under each
// kind of age), the pre-flight check, the workbooks, and an upload in two
// steps. `prepare_upload` signs in, asks who the account is and what the
// package is, builds the submissions and checks their terms; the session, the
// lookups it has made and the submissions are then kept for `upload`, which
// sends exactly what was shown and asks for no term again, or dropped by
// `abandon_upload`.
//
// The thread waits on the outside world and is not a clock participant
// (AGENTS.md, "Time"). An owner that goes away with a request still out does
// not wait for it: the thread finishes that request, drops the rest and ends
// on its own. Not dropped: a password to keep or to forget, which the owner
// asked for on its way out (OK in Preferences) and is owed.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QObject>
#include <QString>
#include <QStringList>

#include "pychron/earthbank/http.hpp"
#include "pychron/earthbank/mapping.hpp"
#include "pychron/earthbank/preflight.hpp"
#include "pychron/earthbank/profiles.hpp"
#include "pychron/earthbank/secret_store.hpp"
#include "pychron/earthbank/session.hpp"
#include "pychron/earthbank/uploader.hpp"
#include "pychron/earthbank/vocab.hpp"
#include "pychron/earthbank/xlsx.hpp"
#include "pychron/processing/model.hpp"

class QThread;

namespace pychron::ui {

// What the EarthBank pages work with. `system()` is the real thing: the
// client over HTTPS (null in a build without Qt Network), the system's
// password store and the profiles file `elctl` reads too. Tests give a
// service in memory, a memory store and a file in a temp folder. Both
// factories are called when they are first needed, never before.
struct EarthbankSetup {
  std::function<std::unique_ptr<earthbank::IHttpClient>()> http;
  std::function<std::shared_ptr<earthbank::ISecretStore>()> secrets;
  std::filesystem::path profiles_file;

  static EarthbankSetup system();
};

// `Passwords are kept in: keychain.` or, for a store that forgets,
// `Passwords are not stored: this session only.`
QString earthbank_store_line(const earthbank::ISecretStore& store);

// One kind of summary age for one sample: the age it would report, in Ma at
// one sigma, or why that kind cannot be computed for it.
struct EarthbankCandidate {
  earthbank::AgeKind kind = earthbank::AgeKind::Plateau;
  std::optional<double> age, uncertainty;
  QString unavailable;
};

// One sample of the selection, as the survey found it.
struct EarthbankSampleRow {
  QString sample;
  int analyses = 0;
  // Why nothing can be published for it under any kind of age (its analyses
  // disagree about what it is, none has an age); empty when something can.
  QString refused;
  // The four kinds, in AgeKind's order.
  std::vector<EarthbankCandidate> candidates;
  // The kind the rule reports (mapping.hpp, PreferredAge); nothing when the
  // rule leaves it to the lab, and `rule_says` is then why.
  std::optional<earthbank::AgeKind> rule;
  QString rule_says;
  QString flux_monitor;  // as found on the analyses
  QStringList notes;     // the mapping's, under the rule
};

// What is published and how: the analyses of the samples taking part, and
// what the lab says of them.
struct EarthbankPublication {
  std::vector<processing::AnalysisPtr> analyses;
  earthbank::PublishOptions options;
};

// What an upload is about to do, as the service has it, for the user to
// confirm. `blocked` not empty: the pre-flight stops it, nothing was kept and
// `upload` has nothing to send.
struct EarthbankConfirmation {
  struct Sample {
    QString name;
    std::size_t aliquots = 0, measurements = 0;
  };
  QString profile, base_url;
  earthbank::Account account;
  earthbank::DataPackage package;
  std::vector<Sample> samples;
  earthbank::PreflightReport preflight;
  QString blocked;
};

struct EarthbankUploadSettings {
  std::optional<std::int64_t> package;  // nothing: the profile's
  bool allow_unresolved = false;
  bool new_datapoint = false;
  std::optional<std::string> resolve_in_doubt;  // uploader.hpp
  std::filesystem::path journal_dir;
};

class EarthbankWorker : public QObject {
  Q_OBJECT

 public:
  // `http` is called on the worker's thread, once, at the first request that
  // needs the service.
  EarthbankWorker(std::function<std::unique_ptr<earthbank::IHttpClient>()> http,
                  std::shared_ptr<earthbank::ISecretStore> secrets, QObject* parent = nullptr);
  ~EarthbankWorker() override;
  EarthbankWorker(const EarthbankWorker&) = delete;
  EarthbankWorker& operator=(const EarthbankWorker&) = delete;

  // Signs in and asks who the account is: `tested`, or `failed`.
  void test(const earthbank::Profile& profile, const QString& password);
  // The packages of the profile's institution (every package when it names
  // none): `packages_listed`, or `failed`.
  void list_packages(const earthbank::Profile& profile, const QString& password);
  // A new package of that institution: `package_created`, or `failed`.
  void create_package(const earthbank::Profile& profile, const QString& password, const QString& name);
  // Keeps, or forgets, the profile's password without asking the service:
  // `password_kept` either way, with what went wrong when it did.
  void store_password(const earthbank::Profile& profile, const QString& password);
  void erase_password(const earthbank::Profile& profile);

  // ---- Publishing ----------------------------------------------------------
  // What each sample of `publication` would report: `surveyed`, with
  // `progress` on the way. Asks the service nothing.
  void survey(const EarthbankPublication& publication);
  // Signs in and checks every term: `preflighted`, or `failed`.
  void preflight(const earthbank::Profile& profile, const QString& password, const EarthbankPublication& publication);
  // Writes the workbooks into `folder`: `exported`, or `failed`. Asks the
  // service nothing.
  void export_xlsx(const EarthbankPublication& publication, const std::filesystem::path& folder,
                   const earthbank::XlsxOptions& options);
  // `upload_prepared`, or `failed`; then `upload` (`progress` for each
  // record, `uploaded` at the end) or `abandon_upload`.
  void prepare_upload(const earthbank::Profile& profile, const QString& password,
                      const EarthbankPublication& publication, const EarthbankUploadSettings& settings);
  void upload();
  void abandon_upload();
  // Stops a survey between samples and an upload between records.
  void cancel();

  // Requests asked for and not yet answered.
  int outstanding() const noexcept { return outstanding_; }

 signals:
  // `note`: empty, or why the password that signed in was not kept.
  void tested(const QString& profile, const QString& name, const QString& login, const QString& note);
  void packages_listed(const QString& profile, const std::vector<pychron::earthbank::DataPackage>& packages);
  void package_created(const QString& profile, const pychron::earthbank::DataPackage& package);
  // A request that asked the service for something and did not get it:
  // `test` and the publishing requests.
  void failed(const QString& profile, const QString& what);
  // The same of `list_packages` and, with `creating`, of `create_package`:
  // theirs alone, so that a list that fails late is not taken for the
  // failure of whatever was asked for next.
  void package_failed(const QString& profile, const QString& what, bool creating);
  void password_kept(const QString& profile, const QString& problem);
  void progress(int done, int total, const QString& what);
  void surveyed(const std::vector<pychron::ui::EarthbankSampleRow>& rows);
  void preflighted(const pychron::earthbank::PreflightReport& report);
  void exported(const pychron::earthbank::XlsxResult& result);
  void upload_prepared(const pychron::ui::EarthbankConfirmation& confirmation);
  void uploaded(const pychron::earthbank::UploadReport& report);
  // After every answer, with what is still outstanding.
  void outstanding_changed(int outstanding);

 private slots:
  // Runs, on this object's thread, one answer the worker's thread has left.
  void deliver();

 private:
  class Job;
  using Reply = std::function<void()>;
  // Runs `work` on the worker's thread; what it returns runs here.
  // `must_run`: it is done even if this object is gone by then.
  void ask(std::function<Reply(Job&)> work, bool must_run = false);

  QThread* thread_;
  Job* job_;  // lives on thread_, which deletes it when it ends
  // Where the thread leaves its answers for this object (a Mailbox, in the
  // .cpp), shared with the thread: it outlives whichever of the two goes first.
  std::shared_ptr<void> mailbox_;
  int outstanding_ = 0;
};

}  // namespace pychron::ui
