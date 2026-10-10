#include "pychron/experiment/store/store_persister.hpp"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <utility>

#include "pychron/core/env.hpp"
#include "pychron/experiment/store/record_ingest.hpp"
#include "pychron/persistence/catalog.hpp"

namespace pychron::experiment::store {

namespace ps = pychron::persistence;

namespace {

// Adds to the store's refusal of an unknown row what the operator does about
// it. The store's own words stay first: is_unknown_catalog_reference() reads
// the start of the text, and a caller must still be able to tell the error.
Error with_advice(Error error) {
  if (ps::is_unknown_catalog_reference(error))
    error.what += ": not in the database. Add it (elctl entry ...) and start a queue to send the run again";
  return error;
}

// A repeated ingest is a success: the store had the item already.
Result<void> ingest(ps::IStore& store, const ps::IngestItem& item) {
  auto ack = store.ingest(item);
  if (!ack) return fail(with_advice(ack.error()));
  return {};
}

}  // namespace

StorePersister::StorePersister(ps::IStore& store, ps::Uuid client, std::filesystem::path data)
    : store_(store), client_(client), data_(std::move(data)), files_(data_ / "records") {}

Result<int> StorePersister::next_aliquot(const std::string& identifier) {
  int highest = 0;
  auto last = store_.last_analysis(identifier);
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (last) {
      highest = *last ? (*last)->aliquot : 0;
      read_[identifier] = highest;
    } else if (const auto cached = read_.find(identifier); cached != read_.end()) {
      highest = cached->second;
    } else {
      return fail(last.error());
    }
    if (const auto issued = issued_.find(identifier); issued != issued_.end()) highest = std::max(highest, issued->second);
  }
  // A record waiting to be sent has its aliquot already; the file read stays outside the lock.
  const persist::Spool spool(data_ / "spool");
  if (auto waiting = spool.pending()) {
    for (const auto& uuid : *waiting)
      if (auto rec = spool.get(uuid); rec && rec->identity.identifier == identifier)
        highest = std::max(highest, rec->identity.aliquot);
  }
  const std::lock_guard<std::mutex> lock(mutex_);
  int& issued = issued_[identifier];
  issued = std::max(issued, highest + 1);
  return highest + 1;
}

Result<void> StorePersister::begin_run(const RunIdentity& id, const QueueSpec& queue) {
  return files_.begin_run(id, queue);
}

Result<void> StorePersister::save_extraction(const record::AnalysisRecord& record) {
  return files_.save_extraction(record);
}

Result<void> StorePersister::save_artifact(const std::string& uuid, const std::string& name,
                                           const std::vector<std::uint8_t>& bytes) {
  return files_.save_artifact(uuid, name, bytes);
}

Result<void> StorePersister::save_analysis(const record::AnalysisRecord& record) {
  auto items = to_ingest(record, client_);
  if (!items) return fail(items.error());
  for (const auto& blob : items->blobs)
    if (auto r = ingest(store_, blob); !r) return r;
  return ingest(store_, items->analysis);
}

Result<std::vector<std::string>> StorePersister::check_queue(const QueueSpec& queue) {
  std::vector<std::string> lines;
  std::optional<Error> unreachable;
  // True when the row is there. A read that fails ends the check (see below).
  const auto has_row = [&](ps::CatalogTable table, const std::string& name) {
    auto found = store_.find_catalog_row(table, {name});
    if (!found) {
      unreachable = found.error();
      return true;
    }
    return found->has_value();
  };
  const auto finish = [&]() -> Result<std::vector<std::string>> {
    if (unreachable) return std::vector<std::string>{"the database cannot be reached: " + unreachable->what};
    return lines;
  };

  if (queue.mass_spectrometer.empty()) {
    lines.emplace_back("the queue names no mass spectrometer");
  } else if (!has_row(ps::CatalogTable::MassSpectrometer, queue.mass_spectrometer)) {
    lines.push_back("mass spectrometer '" + queue.mass_spectrometer + "' is not in the database");
  }
  if (unreachable) return finish();

  std::set<std::string> seen;
  const auto check_device = [&](const std::string& name) {
    if (name.empty() || unreachable || !seen.insert(name).second) return;
    if (!has_row(ps::CatalogTable::ExtractDevice, name)) lines.push_back("extract device '" + name + "' is not in the database");
  };
  check_device(queue.extract_device);
  for (const auto& run : queue.runs) check_device(run.extraction.device);
  if (unreachable) return finish();

  seen.clear();
  for (const auto& run : queue.runs) {
    const std::string& identifier = run.id.identifier;
    if (run.skip || identifier.empty() || !seen.insert(identifier).second) continue;
    auto found = store_.find_identifier(identifier);
    if (!found) {
      unreachable = found.error();
      return finish();
    }
    if (!*found) lines.push_back("identifier '" + identifier + "' is not in the database");
  }

  for (const auto& run : queue.runs) {
    if (run.skip || !run.id.aliquot) continue;
    const auto increment = increment_of(run.id.step);
    if (!increment) continue;  // a step the store cannot hold; the run fails when it is saved
    auto found = store_.find_analysis(run.id.identifier, *run.id.aliquot, *increment);
    if (!found) {
      unreachable = found.error();
      return finish();
    }
    if (*found) lines.push_back(ps::make_runid(run.id.identifier, *run.id.aliquot, *increment) + " is already in the database");
  }
  return lines;
}

std::string without_password(const std::string& url) {
  constexpr std::array<std::string_view, 2> kSchemes{"postgresql://", "postgres://"};
  for (const auto scheme : kSchemes) {
    if (!url.starts_with(scheme)) continue;
    const std::size_t start = scheme.size();
    const std::size_t end = std::min(url.find('/', start), url.size());
    const std::size_t at = url.rfind('@', end);
    if (at == std::string::npos || at < start) return url;
    const std::size_t colon = url.find(':', start);
    if (colon == std::string::npos || colon > at) return url;
    return url.substr(0, colon) + url.substr(at);
  }
  return url;
}

// ---------------------------------------------------------------- Acquisition

// Everything the worker and its callers share. Kept for the life of the
// process (see keep()), so that a call made after the Acquisition is gone
// finds a mutex and a closed flag, not freed memory. What it holds that costs
// anything (the store, the persister) is released on the worker at shutdown.
struct AcquisitionCore;

namespace {

class Marshalled final : public persist::IAnalysisPersister {
 public:
  explicit Marshalled(AcquisitionCore& core) : core_(core) {}

  Result<int> next_aliquot(const std::string& identifier) override;
  Result<void> begin_run(const RunIdentity& id, const QueueSpec& queue) override;
  Result<void> save_extraction(const record::AnalysisRecord& record) override;
  Result<void> save_analysis(const record::AnalysisRecord& record) override;
  Result<void> save_artifact(const std::string& uuid, const std::string& name,
                             const std::vector<std::uint8_t>& bytes) override;
  Result<void> flush() override;
  Result<std::vector<std::string>> check_queue(const QueueSpec& queue) override;

 private:
  AcquisitionCore& core_;
};

}  // namespace

struct AcquisitionCore {
  struct Job {
    std::function<void()> run;
    bool done = false;
  };

  std::mutex mutex;
  std::condition_variable cv;
  std::deque<Job*> queue;
  bool closed = false;
  bool opened = false;
  std::optional<Error> open_error;
  std::thread worker;

  // Touched by the worker only.
  std::unique_ptr<ps::IStore> store;
  std::unique_ptr<StorePersister> persister;

  Marshalled proxy{*this};

  // Runs `f` on the worker; the caller waits for it.
  template <class R, class F>
  R call(F&& f) {
    R result = fail(ErrorKind::Io, "the database connection is closed");
    Job job{[&] { result = f(*persister); }, false};
    std::unique_lock<std::mutex> lock(mutex);
    if (closed) return result;
    // cppcheck-suppress danglingLifetime ; the worker takes the job off the queue before it marks it done, and this call waits for that
    queue.push_back(&job);
    cv.notify_all();
    cv.wait(lock, [&] { return job.done; });
    return result;
  }

  void work(const std::string& url, std::filesystem::path data, const std::string& software_version) {
    {
      auto opened_store = ps::open_store(ps::StoreConfig{url, false});
      Result<void> ready;
      if (!opened_store) {
        Error error = opened_store.error();
        if (error.kind == ErrorKind::Config && error.what.find("schema is out of date") != std::string::npos)
          error.what += "\nRun: elctl db migrate --db " + without_password(url);
        ready = fail(std::move(error));
      } else {
        store = std::move(*opened_store);
        auto client = store->register_client(
            {env_var("HOSTNAME").value_or("localhost"), "acquisition", std::nullopt, software_version});
        if (client)
          persister = std::make_unique<StorePersister>(*store, *client, std::move(data));
        else
          ready = fail(client.error());
      }
      const std::lock_guard<std::mutex> lock(mutex);
      opened = true;
      if (!ready) {
        open_error = ready.error();
        closed = true;
      }
      cv.notify_all();
      if (!ready) {
        // Nothing was queued: callers see `closed` before they queue.
        persister.reset();
        store.reset();
        return;
      }
    }
    std::unique_lock<std::mutex> lock(mutex);
    while (true) {
      cv.wait(lock, [&] { return closed || !queue.empty(); });
      if (queue.empty()) break;  // closed, and everything queued has been served
      Job* job = queue.front();
      queue.pop_front();
      lock.unlock();
      job->run();
      lock.lock();
      job->done = true;
      cv.notify_all();
    }
    lock.unlock();
    persister.reset();
    store.reset();
  }
};

namespace {

// One per Acquisition ever opened; a program opens one.
AcquisitionCore& keep(std::unique_ptr<AcquisitionCore> core) {
  static std::mutex mutex;
  static std::vector<std::unique_ptr<AcquisitionCore>> cores;
  const std::lock_guard<std::mutex> lock(mutex);
  cores.push_back(std::move(core));
  return *cores.back();
}

Result<int> Marshalled::next_aliquot(const std::string& identifier) {
  return core_.call<Result<int>>([&](StorePersister& p) { return p.next_aliquot(identifier); });
}
Result<void> Marshalled::begin_run(const RunIdentity& id, const QueueSpec& queue) {
  return core_.call<Result<void>>([&](StorePersister& p) { return p.begin_run(id, queue); });
}
Result<void> Marshalled::save_extraction(const record::AnalysisRecord& record) {
  return core_.call<Result<void>>([&](StorePersister& p) { return p.save_extraction(record); });
}
Result<void> Marshalled::save_analysis(const record::AnalysisRecord& record) {
  return core_.call<Result<void>>([&](StorePersister& p) { return p.save_analysis(record); });
}
Result<void> Marshalled::save_artifact(const std::string& uuid, const std::string& name,
                                       const std::vector<std::uint8_t>& bytes) {
  return core_.call<Result<void>>([&](StorePersister& p) { return p.save_artifact(uuid, name, bytes); });
}
Result<void> Marshalled::flush() {
  return core_.call<Result<void>>([&](StorePersister& p) { return p.flush(); });
}
Result<std::vector<std::string>> Marshalled::check_queue(const QueueSpec& queue) {
  return core_.call<Result<std::vector<std::string>>>([&](StorePersister& p) { return p.check_queue(queue); });
}

}  // namespace

Acquisition::~Acquisition() {
  {
    const std::lock_guard<std::mutex> lock(core_.mutex);
    core_.closed = true;
    core_.cv.notify_all();
  }
  if (core_.worker.joinable()) core_.worker.join();
}

persist::IAnalysisPersister& Acquisition::persister() noexcept { return core_.proxy; }

Result<std::unique_ptr<Acquisition>> open_acquisition(const std::string& url, std::filesystem::path data,
                                                      const std::string& software_version) {
  AcquisitionCore& core = keep(std::make_unique<AcquisitionCore>());
  core.worker = std::thread([&core, url, data = std::move(data), software_version]() mutable {
    core.work(url, std::move(data), software_version);
  });
  {
    std::unique_lock<std::mutex> lock(core.mutex);
    core.cv.wait(lock, [&] { return core.opened; });
  }
  if (core.open_error) {
    core.worker.join();
    return fail(*core.open_error);
  }
  return std::unique_ptr<Acquisition>(new Acquisition(core));
}

}  // namespace pychron::experiment::store
