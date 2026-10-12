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
// The thread waits on the outside world and is not a clock participant
// (AGENTS.md, "Time"). An owner that goes away with a request still out does
// not wait for it: the thread finishes that request, drops the rest and ends
// on its own.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

#include <QObject>
#include <QString>

#include "pychron/earthbank/http.hpp"
#include "pychron/earthbank/profiles.hpp"
#include "pychron/earthbank/secret_store.hpp"
#include "pychron/earthbank/session.hpp"

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

  // Requests asked for and not yet answered.
  int outstanding() const noexcept { return outstanding_; }

 signals:
  // `note`: empty, or why the password that signed in was not kept.
  void tested(const QString& profile, const QString& name, const QString& login, const QString& note);
  void packages_listed(const QString& profile, const std::vector<pychron::earthbank::DataPackage>& packages);
  void package_created(const QString& profile, const pychron::earthbank::DataPackage& package);
  // A request that asked the service for something and did not get it.
  void failed(const QString& profile, const QString& what);
  void password_kept(const QString& profile, const QString& problem);
  // After every answer, with what is still outstanding.
  void outstanding_changed(int outstanding);

 private:
  class Job;
  using Reply = std::function<void()>;
  // Runs `work` on the worker's thread; what it returns runs here.
  void ask(std::function<Reply(Job&)> work);

  QThread* thread_;
  Job* job_;  // lives on thread_, which deletes it when it ends
  // Where the thread leaves its answers for this object (a Mailbox, in the
  // .cpp), shared with the thread: it outlives whichever of the two goes first.
  std::shared_ptr<void> mailbox_;
  int outstanding_ = 0;
};

}  // namespace pychron::ui
