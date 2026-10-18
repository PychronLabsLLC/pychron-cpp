#include "pychron/experiment/store/store_persister.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <exception>
#include <string_view>
#include <vector>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <tuple>
#include <utility>

#include "pychron/core/host_name.hpp"
#include "pychron/experiment/factory/increments.hpp"
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

// The store's answer as a LastRun (increment is the step's index; none: aliquot 0).
persist::LastRun to_last_run(const std::optional<ps::LastAnalysis>& last) {
  if (!last) return persist::LastRun{0, -1};
  return persist::LastRun{last->aliquot, last->increment};
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
      const persist::LastRun read = to_last_run(*last);
      read_[identifier] = read;
      highest = read.aliquot;
    } else if (const auto cached = read_.find(identifier); cached != read_.end()) {
      highest = cached->second.aliquot;
    } else {
      return fail(last.error());
    }
    if (const auto issued = issued_.find(identifier); issued != issued_.end()) highest = std::max(highest, issued->second);
  }
  // A record waiting to be sent has its aliquot already; the file read stays outside the lock.
  const persist::Spool spool(data_ / "spool");
  auto waiting = spool.pending();  // a missing folder is an empty spool, not an error
  if (!waiting) return fail(waiting.error());
  for (const auto& uuid : *waiting) {
    // A record that cannot be read can never be sent, so its number is not in use.
    if (auto rec = spool.get(uuid); rec && rec->identity.identifier == identifier)
      highest = std::max(highest, rec->identity.aliquot);
  }
  const std::lock_guard<std::mutex> lock(mutex_);
  int& issued = issued_[identifier];
  issued = std::max(issued, highest + 1);
  return highest + 1;
}

Result<std::optional<persist::LastRun>> StorePersister::last_run(const std::string& identifier) {
  persist::LastRun best;
  auto last = store_.last_analysis(identifier);
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (last) {
      best = to_last_run(*last);
      read_[identifier] = best;
    } else if (const auto cached = read_.find(identifier); cached != read_.end()) {
      best = cached->second;
    } else {
      return fail(last.error());
    }
  }
  // A record waiting to be sent is a saved run too; the file read stays outside the lock.
  const persist::Spool spool(data_ / "spool");
  auto waiting = spool.pending();  // a missing folder is an empty spool, not an error
  if (!waiting) return fail(waiting.error());
  for (const auto& uuid : *waiting) {
    // A record that cannot be read can never be sent, so it is no saved run.
    auto rec = spool.get(uuid);
    if (!rec || rec->identity.identifier != identifier) continue;
    const persist::LastRun run{rec->identity.aliquot, step_index(rec->identity.step)};
    if (std::tie(run.aliquot, run.step) > std::tie(best.aliquot, best.step)) best = run;
  }
  if (best.aliquot == 0) return std::optional<persist::LastRun>{};
  return std::optional<persist::LastRun>{best};
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

  // A step the store cannot hold (record_ingest.hpp) would be measured and
  // then refused for good: known now, said now.
  seen.clear();
  for (const auto& run : queue.runs) {
    if (run.skip || increment_of(run.id.step)) continue;
    if (!seen.insert(run.id.identifier + "\n" + run.id.step).second) continue;
    lines.push_back(run.id.identifier + ": step '" + run.id.step + "' is not letters: the database cannot hold it");
  }

  std::set<std::string> taken_seen;
  for (const auto& run : queue.runs) {
    if (run.skip || !run.id.aliquot) continue;
    const auto increment = increment_of(run.id.step);
    if (!increment) continue;  // a step the store cannot hold: said above
    const std::string runid = ps::make_runid(run.id.identifier, *run.id.aliquot, *increment);
    if (!taken_seen.insert(runid).second) continue;
    auto found = store_.find_analysis(run.id.identifier, *run.id.aliquot, *increment);
    if (!found) {
      unreachable = found.error();
      return finish();
    }
    if (*found) lines.push_back(runid + " is already in the database");
  }
  return lines;
}

bool saves_simulated_runs(std::string_view url, bool from_db_option, bool sim_save) {
  if (!url.starts_with("sqlite:") && !url.starts_with("postgresql://") && !url.starts_with("postgres://")) return false;
  if (sim_save) return true;
  return !from_db_option && url.starts_with("sqlite:");
}

namespace {

constexpr std::array<std::string_view, 2> kPostgresSchemes{"postgresql://", "postgres://"};

// A postgres url as it may be shown, and what of it must not be.
struct Shown {
  std::string url;
  std::vector<std::string> secrets;  // as written in the url
};

bool secret_parameter(std::string_view key) {
  std::string lower(key);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower == "password" || lower == "sslpassword";
}

// scheme://[user[:password]@]host[:port][/database][?parameter=value&...]
//
// The user's part ends at the last `@` before the parameters, so a password
// may hold `@`, `/` and `:` as they are; when no `@` stands before the first
// `?`, at the last `@` of all (a password may hold a `?` too). Where that
// guess is wrong (an `@` in a parameter of a url with no user) more is left
// out than need be, never less. nullopt: not a postgres url.
std::optional<Shown> show_postgres(std::string_view url) {
  // NOLINTNEXTLINE(readability-qualified-auto): an array's iterator is a pointer only in some standard libraries
  const auto scheme = std::find_if(kPostgresSchemes.begin(), kPostgresSchemes.end(),
                                   [&](std::string_view s) { return url.starts_with(s); });
  if (scheme == kPostgresSchemes.end()) return std::nullopt;
  std::string_view rest = url.substr(scheme->size());
  Shown out;
  out.url = std::string(*scheme);

  std::size_t at = rest.substr(0, rest.find('?')).rfind('@');
  if (at == std::string_view::npos) at = rest.rfind('@');
  if (at != std::string_view::npos) {
    const std::string_view userinfo = rest.substr(0, at);
    const std::size_t colon = userinfo.find(':');
    const std::string_view user = userinfo.substr(0, colon);
    if (colon != std::string_view::npos) out.secrets.emplace_back(userinfo.substr(colon + 1));
    if (!user.empty()) {
      out.url += user;
      out.url += '@';
    }
    rest = rest.substr(at + 1);
  }

  const std::size_t query = rest.find('?');
  out.url += rest.substr(0, query);
  if (query == std::string_view::npos) return out;
  std::string kept;
  std::string_view parameters = rest.substr(query + 1);
  while (!parameters.empty()) {
    const std::size_t amp = parameters.find('&');
    const std::string_view parameter = parameters.substr(0, amp);
    parameters = amp == std::string_view::npos ? std::string_view{} : parameters.substr(amp + 1);
    const std::size_t equals = parameter.find('=');
    if (secret_parameter(parameter.substr(0, equals))) {
      if (equals != std::string_view::npos) out.secrets.emplace_back(parameter.substr(equals + 1));
      continue;
    }
    if (parameter.empty()) continue;
    kept += kept.empty() ? '?' : '&';
    kept += parameter;
  }
  out.url += kept;
  return out;
}

// %41 -> A; a `%` not followed by two hex digits stays.
std::string percent_decoded(std::string_view text) {
  const auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size() && hex(text[i + 1]) >= 0 && hex(text[i + 2]) >= 0) {
      out += static_cast<char>(hex(text[i + 1]) * 16 + hex(text[i + 2]));
      i += 2;
    } else {
      out += text[i];
    }
  }
  return out;
}

void replace_all(std::string& text, std::string_view what, std::string_view with) {
  if (what.empty()) return;
  for (std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + with.size()))
    text.replace(at, what.size(), with);
}

}  // namespace

std::string without_password(const std::string& url) {
  if (url.starts_with("sqlite:")) return url;  // a file's path, or :memory:
  if (auto shown = show_postgres(url)) return std::move(shown->url);
  return "<database>";  // not understood: not shown
}

std::string without_password_in(std::string text, const std::string& url) {
  if (url.empty() || url.starts_with("sqlite:")) return text;
  const auto shown = show_postgres(url);
  replace_all(text, url, shown ? std::string_view(shown->url) : std::string_view("<database>"));
  if (!shown) return text;
  // A shorter password would be found in ordinary words.
  constexpr std::size_t kShortest = 4;
  for (const auto& secret : shown->secrets) {
    const std::string decoded = percent_decoded(secret);
    if (secret.size() >= kShortest) replace_all(text, secret, "***");
    if (decoded != secret && decoded.size() >= kShortest) replace_all(text, decoded, "***");
  }
  return text;
}

// ---------------------------------------------------------------- Acquisition

struct AcquisitionImpl {
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

  // The url the store was opened with: taken out of every error text.
  std::string url;

  // Runs `f` on the worker; the caller waits for it. Not to be called from the
  // worker itself (it would wait on itself); nothing does.
  template <class R, class F>
  R call(F&& f) {
    R result = fail(ErrorKind::Io, "the database connection is closed");
    Job job{[&] {
              // The one catch of this library, at the boundary to a thread:
              // errors are values everywhere here, but whatever a call may
              // still throw (a library's exception, bad_alloc) would
              // otherwise leave the worker and end the program in the
              // middle of a queue. It is that call's error instead.
              try {
                result = f(*persister);
              } catch (const std::exception& e) {
                result = fail(ErrorKind::Io, std::string("the database call failed unexpectedly: ") + e.what());
              } catch (...) {
                result = fail(ErrorKind::Io, "the database call failed unexpectedly");
              }
              if (!result) {
                Error error = result.error();
                error.what = without_password_in(std::move(error.what), url);
                result = fail(std::move(error));
              }
            },
            false};
    std::unique_lock<std::mutex> lock(mutex);
    // cppcheck-suppress danglingLifetime ; the worker takes the job off the queue before it marks it done, and this call waits for that
    queue.push_back(&job);
    cv.notify_all();
    cv.wait(lock, [&] { return job.done; });
    return result;
  }

  // Opens the store and the persister; the error, if any.
  std::optional<Error> open(std::filesystem::path data, const std::string& software_version) {
    auto opened_store = ps::open_store(ps::StoreConfig{url, false});
    if (!opened_store) {
      Error error = opened_store.error();
      if (error.kind == ErrorKind::Config && error.what.find("schema is out of date") != std::string::npos)
        error.what += "\nRun: elctl db migrate --db " + without_password(url);
      return error;
    }
    store = std::move(*opened_store);
    auto client = store->register_client(
        {host_name(), "acquisition", std::nullopt, software_version});
    if (!client) return client.error();
    persister = std::make_unique<StorePersister>(*store, *client, std::move(data));
    return std::nullopt;
  }

  void work(std::filesystem::path data, const std::string& software_version) {
    auto error = open(std::move(data), software_version);
    if (error) error->what = without_password_in(std::move(error->what), url);
    {
      const std::lock_guard<std::mutex> lock(mutex);
      opened = true;
      open_error = error;
    }
    cv.notify_all();
    if (error) {
      persister.reset();
      store.reset();
      return;
    }
    std::unique_lock<std::mutex> lock(mutex);
    while (true) {
      cv.wait(lock, [&] { return closed || !queue.empty(); });
      if (queue.empty()) break;  // closed, and everything queued has been served
      Job* job = queue.front();
      queue.pop_front();
      lock.unlock();
      job->run();  // never throws: call() catches what the call throws
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

class Marshalled final : public persist::IAnalysisPersister {
 public:
  explicit Marshalled(AcquisitionImpl& impl) : impl_(impl) {}

  Result<int> next_aliquot(const std::string& identifier) override {
    return impl_.call<Result<int>>([&](StorePersister& p) { return p.next_aliquot(identifier); });
  }
  Result<std::optional<persist::LastRun>> last_run(const std::string& identifier) override {
    return impl_.call<Result<std::optional<persist::LastRun>>>(
        [&](StorePersister& p) { return p.last_run(identifier); });
  }
  Result<void> begin_run(const RunIdentity& id, const QueueSpec& queue) override {
    return impl_.call<Result<void>>([&](StorePersister& p) { return p.begin_run(id, queue); });
  }
  Result<void> save_extraction(const record::AnalysisRecord& record) override {
    return impl_.call<Result<void>>([&](StorePersister& p) { return p.save_extraction(record); });
  }
  Result<void> save_analysis(const record::AnalysisRecord& record) override {
    return impl_.call<Result<void>>([&](StorePersister& p) { return p.save_analysis(record); });
  }
  Result<void> save_artifact(const std::string& uuid, const std::string& name,
                             const std::vector<std::uint8_t>& bytes) override {
    return impl_.call<Result<void>>([&](StorePersister& p) { return p.save_artifact(uuid, name, bytes); });
  }
  Result<void> flush() override {
    return impl_.call<Result<void>>([&](StorePersister& p) { return p.flush(); });
  }
  Result<std::vector<std::string>> check_queue(const QueueSpec& queue) override {
    return impl_.call<Result<std::vector<std::string>>>([&](StorePersister& p) { return p.check_queue(queue); });
  }

 private:
  AcquisitionImpl& impl_;
};

}  // namespace

Acquisition::Acquisition(std::unique_ptr<AcquisitionImpl> impl) : impl_(std::move(impl)) {
  proxy_ = std::make_unique<Marshalled>(*impl_);
}

Acquisition::~Acquisition() {
  {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->closed = true;
  }
  impl_->cv.notify_all();
  if (impl_->worker.joinable()) impl_->worker.join();
}

persist::IAnalysisPersister& Acquisition::persister() noexcept { return *proxy_; }

Result<std::unique_ptr<Acquisition>> open_acquisition(const std::string& url, std::filesystem::path data,
                                                      const std::string& software_version) {
  ps::ensure_host_application();
  auto impl = std::make_unique<AcquisitionImpl>();
  AcquisitionImpl& core = *impl;
  core.url = url;  // before the worker exists; read by it and by callers, never written again
  core.worker = std::thread([&core, data = std::move(data), software_version]() mutable {
    core.work(std::move(data), software_version);
  });
  {
    std::unique_lock<std::mutex> lock(core.mutex);
    core.cv.wait(lock, [&] { return core.opened; });
  }
  if (core.open_error) {
    core.worker.join();
    return fail(*core.open_error);
  }
  return std::unique_ptr<Acquisition>(new Acquisition(std::move(impl)));
}

}  // namespace pychron::experiment::store
