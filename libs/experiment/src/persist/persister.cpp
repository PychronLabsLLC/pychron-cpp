#include "pychron/experiment/persist/persister.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <string_view>
#include <tuple>
#include <fstream>
#include <sstream>

#include "pychron/experiment/factory/increments.hpp"
#include "pychron/experiment/record/serialize.hpp"

namespace pychron::experiment::persist {

namespace fs = std::filesystem;

namespace {

Unexpected<Error> io(const std::string& what) { return fail(ErrorKind::Io, what); }

bool safe_name(const std::string& s) {
  return !s.empty() && s.find('/') == std::string::npos && s.find('\\') == std::string::npos &&
         s.find("..") == std::string::npos;
}

// Write-then-rename so a crash never leaves a half-written file.
Result<void> write_atomic(const fs::path& path, const std::string& text) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) return io("cannot create " + path.parent_path().string() + ": " + ec.message());
  const fs::path tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return io("cannot write " + tmp.string());
    out << text;
    out.flush();
    if (!out) return io("cannot write " + tmp.string());
  }
  fs::rename(tmp, path, ec);
  if (ec) return io("cannot rename " + tmp.string() + ": " + ec.message());
  return {};
}

Result<std::string> read_text(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return io("cannot read " + path.string());
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// The run a record file name stands for: `<prefix><aliquot><step>.` and
// ending in `.json`, where `prefix` is `<identifier>-`, the aliquot is digits
// and the step letters (none: step -1). Anything else is not a run.
std::optional<LastRun> parse_run_name(std::string_view name, std::string_view prefix) {
  if (!name.starts_with(prefix) || !name.ends_with(".json")) return std::nullopt;
  const std::string_view rest = name.substr(prefix.size());
  const auto digits_end = std::ranges::find_if_not(rest, [](char c) {
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
  });
  const auto n_digits = static_cast<std::size_t>(digits_end - rest.begin());
  if (n_digits == 0) return std::nullopt;
  LastRun run;
  for (const char c : rest.substr(0, n_digits)) {
    const int digit = c - '0';
    if (run.aliquot > (std::numeric_limits<int>::max() - digit) / 10) return std::nullopt;
    run.aliquot = run.aliquot * 10 + digit;
  }
  const std::string_view after = rest.substr(n_digits);
  const auto letters_end = std::ranges::find_if_not(after, [](char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0;
  });
  const auto n_letters = static_cast<std::size_t>(letters_end - after.begin());
  if (n_letters == after.size() || after[n_letters] != '.') return std::nullopt;
  if (n_letters > 0) {
    run.step = step_index(after.substr(0, n_letters));
    if (run.step < 0) return std::nullopt;
  }
  return run;
}

std::string run_name(const record::Identity& id) {
  return id.identifier + "-" + std::to_string(id.aliquot) + id.step;
}

}  // namespace

QueueSpec rows_from(QueueSpec queue, std::size_t from_row) {
  const std::size_t n = std::min(from_row, queue.runs.size());
  for (std::size_t i = 0; i < n; ++i) queue.runs[i].skip = true;
  return queue;
}

// ---- FilePersister ------------------------------------------------------------

int FilePersister::highest_on_disk(const std::string& identifier) const {
  int best = 0;
  std::error_code ec;
  const fs::path dir = root_ / identifier;
  if (!fs::is_directory(dir, ec)) return 0;
  const std::string prefix = identifier + "-";
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    const auto name = e.path().filename().string();
    if (!name.starts_with(prefix) || !name.ends_with(".json")) continue;
    std::size_t i = prefix.size(), j = i;
    while (j < name.size() && std::isdigit(static_cast<unsigned char>(name[j]))) ++j;
    if (j == i) continue;
    best = std::max(best, std::stoi(name.substr(i, j - i)));
  }
  return best;
}

Result<int> FilePersister::next_aliquot(const std::string& identifier) {
  if (!safe_name(identifier)) return fail(ErrorKind::Config, "identifier '" + identifier + "' is not a plain name");
  std::lock_guard lock(mutex_);
  int& issued = issued_[identifier];
  issued = std::max(issued, highest_on_disk(identifier)) + 1;
  return issued;
}

Result<std::optional<LastRun>> FilePersister::last_run(const std::string& identifier) {
  if (!safe_name(identifier)) return fail(ErrorKind::Config, "identifier '" + identifier + "' is not a plain name");
  std::error_code ec;
  const fs::path dir = root_ / identifier;
  if (!fs::is_directory(dir, ec)) return std::optional<LastRun>{};
  const std::string prefix = identifier + "-";
  std::optional<LastRun> best;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const auto run = parse_run_name(it->path().filename().string(), prefix);
    if (!run) continue;
    if (!best || std::tie(run->aliquot, run->step) > std::tie(best->aliquot, best->step)) best = run;
  }
  if (ec) return io("cannot list " + dir.string() + ": " + ec.message());
  return best;
}

Result<void> FilePersister::begin_run(const RunIdentity& id, const QueueSpec&) {
  if (!safe_name(id.identifier)) return fail(ErrorKind::Config, "identifier '" + id.identifier + "' is not a plain name");
  std::error_code ec;
  fs::create_directories(root_ / id.identifier, ec);
  if (ec) return io("cannot create " + (root_ / id.identifier).string() + ": " + ec.message());
  return {};
}

fs::path FilePersister::analysis_path(const record::AnalysisRecord& r) const {
  return root_ / r.identity.identifier / (run_name(r.identity) + ".json");
}

Result<void> FilePersister::save_extraction(const record::AnalysisRecord& r) {
  if (!safe_name(r.identity.identifier)) return fail(ErrorKind::Config, "bad identifier");
  return write_atomic(root_ / r.identity.identifier / (run_name(r.identity) + ".extraction.json"),
                      record::to_json(r));
}

Result<void> FilePersister::save_analysis(const record::AnalysisRecord& r) {
  if (!safe_name(r.identity.identifier)) return fail(ErrorKind::Config, "bad identifier");
  return write_atomic(analysis_path(r), record::to_json(r));
}

Result<void> FilePersister::save_artifact(const std::string& uuid, const std::string& name,
                                          const std::vector<std::uint8_t>& bytes) {
  if (!safe_name(uuid) || !safe_name(name)) return fail(ErrorKind::Config, "artifact names must be plain");
  return write_atomic(root_ / "artifacts" / uuid / name, std::string(bytes.begin(), bytes.end()));
}

// ---- Spool ----------------------------------------------------------------------

Result<void> Spool::put(const record::AnalysisRecord& r) {
  if (!safe_name(r.identity.uuid)) return fail(ErrorKind::Config, "record uuid missing or not plain");
  return write_atomic(dir_ / (r.identity.uuid + ".json"), record::to_json(r));
}

Result<void> Spool::remove(const std::string& uuid) {
  std::error_code ec;
  fs::remove(dir_ / (uuid + ".json"), ec);
  if (ec) return io("cannot remove spooled " + uuid + ": " + ec.message());
  return {};
}

Result<std::vector<std::string>> Spool::pending() const {
  std::error_code ec;
  if (!fs::is_directory(dir_, ec)) return std::vector<std::string>{};
  std::vector<std::pair<fs::file_time_type, std::string>> files;
  // `ec` is the listing's alone. Each file is looked at with a code of its
  // own: one that has gone since it was listed (sent and removed by another
  // call), or that is not a file, is not a pending record, and is no reason
  // to say the spool cannot be listed.
  fs::directory_iterator it(dir_, ec);
  for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
    const fs::path& path = it->path();
    if (path.extension() != ".json") continue;
    std::error_code file_ec;
    if (!fs::is_regular_file(path, file_ec) || file_ec) continue;
    const auto written = fs::last_write_time(path, file_ec);
    if (file_ec) continue;
    files.emplace_back(written, path.stem().string());
  }
  if (ec) return io("cannot list " + dir_.string() + ": " + ec.message());
  std::sort(files.begin(), files.end());
  std::vector<std::string> out;
  for (auto& f : files) out.push_back(std::move(f.second));
  return out;
}

bool Spool::contains(const std::string& uuid) const {
  std::error_code ec;
  return fs::exists(dir_ / (uuid + ".json"), ec) && !ec;
}

Result<record::AnalysisRecord> Spool::get(const std::string& uuid) const {
  auto text = read_text(dir_ / (uuid + ".json"));
  if (!text) return fail(text.error());
  return record::from_json(*text);
}

// ---- SavePipeline ---------------------------------------------------------------

Result<bool> SavePipeline::hand_off(const std::string& uuid) {
  auto rec = spool_.get(uuid);
  // A record that has left the spool since it was listed was sent by a flush
  // or a save on another thread: sending it twice would be harmless (a
  // persister takes a record again as the same analysis), reading what is no
  // longer there is not a failure.
  if (!rec && !spool_.contains(uuid)) return false;
  Result<void> r = rec ? persister_.save_analysis(*rec) : Result<void>(fail(rec.error()));
  if (r) r = spool_.remove(uuid);
  std::lock_guard lock(mutex_);
  if (!r) {
    last_error_ = std::make_pair(uuid, r.error().what);
    return fail(r.error());
  }
  if (last_error_ && last_error_->first == uuid) last_error_.reset();  // the record it spoke of has been sent
  return true;
}

Result<Saved> SavePipeline::save(const record::AnalysisRecord& record) {
  if (auto r = spool_.put(record); !r) return fail(r.error());
  const std::string uuid = record.identity.uuid;
  Saved saved;
  if (post_) {
    post_([this, uuid] { (void)hand_off(uuid); });  // its outcome: pending() and last_error()
  } else if (auto r = hand_off(uuid); !r) {
    saved.pending = r.error();  // the record stays in the spool
  }
  return saved;
}

Result<std::size_t> SavePipeline::flush() {
  auto uuids = spool_.pending();
  if (!uuids) return fail(uuids.error());
  std::size_t done = 0;
  std::optional<Error> first;
  for (const auto& u : *uuids) {
    if (auto r = hand_off(u); !r) {
      if (!first) first = r.error();
    } else if (*r) {
      ++done;
    }
  }
  if (auto r = persister_.flush(); !r && !first) first = r.error();
  if (done == 0 && first) return fail(*first);
  return done;
}

std::size_t SavePipeline::pending() const {
  auto p = spool_.pending();
  return p ? p->size() : 0;
}

std::optional<std::string> SavePipeline::last_error() const {
  std::lock_guard lock(mutex_);
  if (!last_error_) return std::nullopt;
  return last_error_->first + ": " + last_error_->second;
}

// ---- SpoolFlusher ---------------------------------------------------------------

SpoolFlusher::SpoolFlusher(SavePipeline& pipeline, Scheduler& scheduler, const Clock& clock, SignalBus* bus,
                           Options options)
    : pipeline_(pipeline),
      scheduler_(scheduler),
      clock_(clock),
      bus_(bus),
      options_(options),
      delay_(options.interval) {}

SpoolFlusher::~SpoolFlusher() {
  std::vector<JobId> waiting;
  {
    const std::lock_guard lock(mutex_);
    closing_ = true;
    for (const auto& [seq, id] : jobs_) waiting.push_back(id);
  }
  // A job that has begun and not yet taken mutex_ is waited for here; it
  // sees closing_ and returns.
  for (const JobId id : waiting) (void)scheduler_.cancel_and_wait(id);
  std::unique_lock lock(mutex_);
  while (running_) clock_.wait(idle_, lock);
}

void SpoolFlusher::schedule_locked(pychron::Duration delay) {
  const std::uint64_t seq = next_seq_++;
  auto id = scheduler_.after("spool.flush", delay, [this, seq] { run(seq); });
  if (id) jobs_.emplace(seq, *id);
}

void SpoolFlusher::flush_now() {
  const std::lock_guard lock(mutex_);
  if (closing_) return;
  if (running_) {
    again_ = true;  // the flush in progress schedules the next one at once
    return;
  }
  // A timed flush still waiting is replaced by this one. One that has begun
  // and not yet reached run()'s lock is not stopped by cancel(): it finds
  // this one's flush running, or done, and asks for no more than again_.
  for (const auto& [seq, id] : jobs_) (void)scheduler_.cancel(id);
  jobs_.clear();
  schedule_locked(pychron::Duration::zero());
}

SpoolStatus SpoolFlusher::status() const {
  const std::lock_guard lock(mutex_);
  return status_;
}

void SpoolFlusher::run(std::uint64_t seq) {
  {
    const std::lock_guard lock(mutex_);
    jobs_.erase(seq);
    if (closing_) return;
    if (running_) {
      again_ = true;
      return;
    }
    running_ = true;
    again_ = false;
  }
  const auto flushed = pipeline_.flush();
  SpoolStatus now{pipeline_.pending(), pipeline_.last_error()};
  if (!flushed && !now.last_error) now.last_error = flushed.error().what;  // the spool or the persister's flush
  bool changed = false;
  {
    const std::lock_guard lock(mutex_);
    if (flushed) delay_ = options_.interval;
    const pychron::Duration next = again_ ? pychron::Duration::zero() : delay_;
    if (!flushed) delay_ = std::min(delay_ * 2, options_.max_interval);
    if (!closing_ && (again_ || now.pending > 0)) schedule_locked(next);
    again_ = false;
    changed = now != status_;
    if (changed) status_ = now;
  }
  if (changed && bus_ != nullptr) bus_->publish(now);
  // Notified under the lock: once it is released the destructor may run.
  const std::lock_guard lock(mutex_);
  if (again_ && !closing_) schedule_locked(pychron::Duration::zero());  // asked for while it published
  again_ = false;
  running_ = false;
  clock_.notify_all(idle_);
}

// ---- AliquotAllocator -----------------------------------------------------------

Result<Allocation> AliquotAllocator::allocate(const RunIdentity& id) {
  const int step = step_index(id.step);
  auto remember = [&](int aliquot) {
    if (step < 0) return;
    const std::lock_guard lock(mutex_);
    last_[id.identifier] = Heating{aliquot, step};
  };
  if (id.aliquot) {
    if (*id.aliquot < 1) return fail(ErrorKind::Config, "aliquot must be >= 1, got " + std::to_string(*id.aliquot));
    remember(*id.aliquot);
    return Allocation{*id.aliquot, id.step};
  }
  if (step > 0) {
    const std::lock_guard lock(mutex_);
    if (auto it = last_.find(id.identifier); it != last_.end() && step > it->second.step) {
      it->second.step = step;
      return Allocation{it->second.aliquot, id.step};
    }
  }
  // Runs of one identifier start one after another, so nothing can come
  // between this lookup and remember() for the same identifier.
  auto next = persister_.next_aliquot(id.identifier);
  if (!next) return fail(next.error());
  remember(*next);
  return Allocation{*next, id.step};
}

}  // namespace pychron::experiment::persist
