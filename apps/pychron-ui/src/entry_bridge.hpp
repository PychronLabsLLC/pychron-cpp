#pragma once

// The entry windows' way to the DVC store (sample and package entry spec,
// section 9). A store connection belongs to the thread that opened it, so the
// bridge owns one worker thread with its own store and runs every call there;
// results come back on the GUI thread. The windows never block on the store.
//
// Writes are made as one client (role "reduction", which may write catalog
// rows, DVC spec 11.2) and one user, registered when the bridge opens.

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <QMetaObject>
#include <QObject>
#include <QPointer>

#include "entry_headers.hpp"
#include "pychron/core/error.hpp"

namespace pychron::ui {

class EntryBridge : public QObject {
  Q_OBJECT

 public:
  struct Options {
    std::string url;   // the store; it is never migrated from here
    std::string user;  // empty: $USER, else "pychron"
    std::string host;  // empty: $HOSTNAME, else "localhost"
  };

  // Opens the store on the worker thread and registers the client and user.
  static Result<std::unique_ptr<EntryBridge>> open(Options options);
  ~EntryBridge() override;
  EntryBridge(const EntryBridge&) = delete;
  EntryBridge& operator=(const EntryBridge&) = delete;

  const persistence::Actor& actor() const noexcept { return actor_; }

  using Job = std::function<void(persistence::IStore&, const persistence::Actor&)>;

  // Runs `fn` on the worker; `done` gets its result on the GUI thread, unless
  // `receiver` was destroyed meanwhile.
  template <class R>
  void run(QObject* receiver, std::function<Result<R>(persistence::IStore&, const persistence::Actor&)> fn,
           std::function<void(Result<R>)> done) {
    // The result is queued to the bridge, which lives on the GUI thread and
    // outlives every window; the receiver is checked there, on the thread
    // that deletes it, so a window closed meanwhile is never called.
    QPointer<QObject> guard(receiver);
    post([this, guard, fn = std::move(fn), done = std::move(done)](persistence::IStore& s,
                                                                   const persistence::Actor& a) {
      auto result = std::make_shared<Result<R>>(fn(s, a));
      QMetaObject::invokeMethod(
          this,
          [guard, done, result] {
            if (guard) done(std::move(*result));
          },
          Qt::QueuedConnection);
    });
  }

  // Blocks until `fn` has run on the worker (tests, and dialogs that must
  // know before they close).
  template <class R>
  Result<R> run_sync(std::function<Result<R>(persistence::IStore&, const persistence::Actor&)> fn) {
    std::optional<Result<R>> out;
    std::mutex m;
    std::condition_variable cv;
    post([&](persistence::IStore& s, const persistence::Actor& a) {
      auto r = fn(s, a);
      std::lock_guard lock(m);
      out.emplace(std::move(r));
      cv.notify_one();
    });
    std::unique_lock lock(m);
    cv.wait(lock, [&] { return out.has_value(); });
    return std::move(*out);
  }

  // Tells every window that the catalog changed (after a write that applied).
  void notify_changed() { Q_EMIT changed(); }

 Q_SIGNALS:
  void changed();

 private:
  EntryBridge() = default;
  void post(Job job);
  void loop(Options options, std::function<void(Result<persistence::Actor>)> opened);

  std::thread worker_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Job> jobs_;
  bool stopping_ = false;
  persistence::Actor actor_;
};

}  // namespace pychron::ui
