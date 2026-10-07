#include "pychron/systems/spectrometer/acquisition.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace pychron::spectrometer {

namespace {

constexpr int kMaxDrainPerPoll = 4096;

double to_seconds(Duration d) { return std::chrono::duration<double>(d).count(); }

bool is_counter(DetectorKind kind) { return kind == DetectorKind::Counter || kind == DetectorKind::Cdd; }

Duration scaled(Duration d, double factor) {
  return Duration(static_cast<Duration::rep>(static_cast<double>(d.count()) * factor));
}

Duration abs_diff(TimePoint a, TimePoint b) { return a > b ? a - b : b - a; }

}  // namespace

// ---- IntensityStream -------------------------------------------------------

IntensityStream::IntensityStream(const Clock& clock, std::size_t capacity)
    : clock_(clock), capacity_(std::max<std::size_t>(capacity, 1)) {}

Result<std::optional<Reading>> IntensityStream::next(Duration timeout) {
  const TimePoint deadline = clock_.now() + timeout;
  std::unique_lock lock(mutex_);
  for (;;) {
    if (!queue_.empty()) {
      Reading r = std::move(queue_.front());
      queue_.pop_front();
      return std::optional<Reading>(std::move(r));
    }
    if (error_) {
      Error e = std::move(*error_);
      error_.reset();
      return pychron::fail(std::move(e));
    }
    if (clock_.now() >= deadline) return std::optional<Reading>{};
    clock_.wait_until(cv_, lock, deadline);
  }
}

std::size_t IntensityStream::size() const {
  std::lock_guard lock(mutex_);
  return queue_.size();
}

std::uint64_t IntensityStream::dropped() const {
  std::lock_guard lock(mutex_);
  return dropped_;
}

void IntensityStream::push(Reading reading) {
  {
    std::lock_guard lock(mutex_);
    if (queue_.size() >= capacity_) {
      queue_.pop_front();
      ++dropped_;
    }
    queue_.push_back(std::move(reading));
  }
  clock_.notify_all(cv_);
}

void IntensityStream::fail(Error error) {
  {
    std::lock_guard lock(mutex_);
    error_ = std::move(error);
  }
  clock_.notify_all(cv_);
}

void IntensityStream::clear() {
  std::lock_guard lock(mutex_);
  queue_.clear();
  error_.reset();
}

// ---- AcquisitionEngine -----------------------------------------------------

Result<std::unique_ptr<AcquisitionEngine>> AcquisitionEngine::create(
    std::vector<IIntensityAcquirer*> acquirers, std::vector<DetectorConfig> detectors,
    Scheduler& scheduler, SignalBus& bus, const Clock& clock) {
  return create(std::move(acquirers), std::move(detectors), scheduler, bus, clock, Options{});
}

Result<std::unique_ptr<AcquisitionEngine>> AcquisitionEngine::create(
    std::vector<IIntensityAcquirer*> acquirers, std::vector<DetectorConfig> detectors,
    Scheduler& scheduler, SignalBus& bus, const Clock& clock, Options options) {
  if (acquirers.empty()) return fail(ErrorKind::Config, "acquisition engine needs at least one acquirer");
  if (std::any_of(acquirers.begin(), acquirers.end(), [](auto* a) { return a == nullptr; }))
    return fail(ErrorKind::Config, "null acquirer");
  if (options.integration <= Duration::zero())
    return fail(ErrorKind::Config, "integration must be positive");

  std::string problems;
  for (const auto& d : detectors) {
    int carriers = 0;
    for (auto* a : acquirers) {
      const auto chans = a->channels();
      if (std::find(chans.begin(), chans.end(), d.channel) != chans.end()) ++carriers;
    }
    if (carriers != 1) {
      problems += "detector '" + d.name + "' channel '" + d.channel + "' is carried by " +
                  std::to_string(carriers) + " acquirers (need exactly 1); ";
    }
  }
  if (!problems.empty()) return fail(ErrorKind::Config, std::move(problems));

  return std::unique_ptr<AcquisitionEngine>(new AcquisitionEngine(
      std::move(acquirers), std::move(detectors), scheduler, bus, clock, options));
}

AcquisitionEngine::AcquisitionEngine(std::vector<IIntensityAcquirer*> acquirers,
                                     std::vector<DetectorConfig> detectors, Scheduler& scheduler,
                                     SignalBus& bus, const Clock& clock, Options options)
    : acquirers_(std::move(acquirers)),
      detectors_(std::move(detectors)),
      acq_dets_(acquirers_.size()),
      scheduler_(scheduler),
      bus_(bus),
      clock_(clock),
      options_(options),
      stream_(std::make_shared<IntensityStream>(clock, options.queue_capacity)),
      integration_(options.integration),
      bins_(acquirers_.size()),
      have_seq_(acquirers_.size(), false),
      last_seq_(acquirers_.size(), 0),
      last_frame_(acquirers_.size()),
      stalled_(acquirers_.size(), false) {
  for (std::size_t di = 0; di < detectors_.size(); ++di) {
    for (std::size_t ai = 0; ai < acquirers_.size(); ++ai) {
      const auto chans = acquirers_[ai]->channels();
      if (std::find(chans.begin(), chans.end(), detectors_[di].channel) != chans.end())
        acq_dets_[ai].push_back(di);
    }
  }
}

AcquisitionEngine::~AcquisitionEngine() { stop(); }

bool AcquisitionEngine::running() const {
  std::lock_guard lock(mutex_);
  return running_;
}

Duration AcquisitionEngine::integration() const {
  std::lock_guard lock(mutex_);
  return integration_;
}

std::uint64_t AcquisitionEngine::dropped_frames() const {
  std::lock_guard lock(mutex_);
  return dropped_frames_;
}

std::uint64_t AcquisitionEngine::stale_frames() const {
  std::lock_guard lock(mutex_);
  return stale_frames_;
}

void AcquisitionEngine::begin_request() {
  const TimePoint now = clock_.now();
  {
    std::lock_guard lock(mutex_);
    request_start_ = now;
    epoch_ = now;
    pending_.clear();
    for (std::size_t i = 0; i < acquirers_.size(); ++i) {
      bins_[i] = Bin{};
      have_seq_[i] = false;
      last_frame_[i] = now;
      stalled_[i] = false;
    }
  }
  stream_->clear();
}

Result<void> AcquisitionEngine::start(Duration integration) {
  if (integration <= Duration::zero()) return fail(ErrorKind::Config, "integration must be positive");
  {
    std::unique_lock lock(mutex_);
    if (polls_on_this_thread() > 0) {
      // The stopper is waiting for this very poll: waiting for it would deadlock.
      if (stopping_) return fail(ErrorKind::Config, "acquisition is stopping", "acquisition");
      // Nor wait, inside a poll, for another thread's start(): that start()
      // or a stop() queued behind it may come to wait for this poll.
      if (starting_) return fail(ErrorKind::Config, "acquisition is starting", "acquisition");
    } else {
      while (!settled()) clock_.wait(polls_cv_, lock);
    }
    if (running_) return fail(ErrorKind::Config, "acquisition already running");
    integration_ = integration;
    starting_ = true;  // other start() and stop() calls wait until this one is done
  }
  // Cleared on every way out, so an early return cannot leave it set.
  struct Starting {
    AcquisitionEngine& engine;
    bool armed = true;
    // Caller holds mutex_. Notifies under the lock: a waiter may destroy the
    // engine once it wakes.
    void clear() {
      armed = false;
      engine.starting_ = false;
      engine.clock_.notify_all(engine.polls_cv_);
    }
    ~Starting() {
      if (!armed) return;
      std::lock_guard lock(engine.mutex_);
      clear();
    }
  };
  Starting starting{*this};

  std::size_t started = 0;
  auto undo = [&] {
    for (std::size_t i = 0; i < started; ++i) (void)acquirers_[i]->stop();
  };
  for (auto* a : acquirers_) {
    if (auto r = a->configure(integration); !r) return fail(r.error());
  }
  begin_request();
  for (auto* a : acquirers_) {
    if (auto r = a->start(); !r) {
      undo();
      return fail(r.error());
    }
    ++started;
  }
  std::vector<JobId> jobs;
  for (std::size_t i = 0; i < acquirers_.size(); ++i) {
    auto id = scheduler_.every("acquisition:" + std::to_string(i), options_.poll_interval,
                               [this, i] { poll(i); });
    if (!id) {
      for (auto j : jobs) scheduler_.cancel(j);
      undo();
      return fail(id.error());
    }
    jobs.push_back(*id);
  }
  std::lock_guard lock(mutex_);
  jobs_ = std::move(jobs);
  running_ = true;
  starting.clear();  // in the same step as running_, so waiters see one or the other
  return {};
}

std::size_t AcquisitionEngine::polls_on_this_thread() const {
  return static_cast<std::size_t>(
      std::count(polling_.begin(), polling_.end(), std::this_thread::get_id()));
}

bool AcquisitionEngine::settled() const {
  return !starting_ && (running_ || (!stopping_ && polling_.empty()));
}

void AcquisitionEngine::stop() {
  std::vector<JobId> jobs;
  {
    std::unique_lock lock(mutex_);
    const std::size_t own = polls_on_this_thread();
    // A start() in progress on another thread is about to bring the engine
    // up: wait for it, then stop what it started. Not from inside a poll (see
    // below).
    if (own == 0) {
      while (starting_) clock_.wait(polls_cv_, lock);
    }
    if (!running_) {
      // Already stopped, perhaps by a stop() still in progress on another
      // thread: return only once that one has stopped the acquirers, or the
      // engine has been started again since. Not from inside a poll, though:
      // that stopper may be waiting for this very poll.
      if (own == 0) {
        while (!settled()) clock_.wait(polls_cv_, lock);
      }
      return;
    }
    running_ = false;
    stopping_ = true;  // start() and other stoppers wait until the acquirers are stopped
    jobs.swap(jobs_);
  }
  for (auto j : jobs) scheduler_.cancel(j);
  {
    // Scheduler::cancel() lets an execution in progress finish; wait for it,
    // without the lock a poll needs to finish. Polls on this thread are our
    // own callers and cannot be waited for. Through the clock, as every wait
    // on polls_cv_ is: the poll may itself be waiting in the clock's time
    // (a driver reading its wire), which passes only while this thread is
    // seen to wait.
    std::unique_lock lock(mutex_);
    while (polling_.size() != polls_on_this_thread()) clock_.wait(polls_cv_, lock);
  }
  for (auto* a : acquirers_) (void)a->stop();
  {
    std::lock_guard lock(mutex_);
    stopping_ = false;
    clock_.notify_all(polls_cv_);
  }
}

void AcquisitionEngine::cancel() {
  {
    std::lock_guard lock(mutex_);
    cancel_ = true;
  }
  clock_.notify_all(collect_cv_);
}

TimePoint AcquisitionEngine::bin_end(std::int64_t index) const {
  return epoch_ + integration_ * (index + 1);
}

Value AcquisitionEngine::make_value(const DetectorConfig& d, double mean) const {
  Value v;
  v.mean = mean * d.software_gain;
  v.saturated = d.saturation && v.mean > *d.saturation;
  return v;
}

std::optional<Value> AcquisitionEngine::finish(const DetectorConfig& d, const Acc& acc) const {
  const auto n = acc.samples.size();
  if (n == 0) return std::nullopt;

  double mean = 0.0;
  bool over_dead_time = false;
  if (is_counter(d.kind)) {
    const double tau = d.dead_time_ns.value_or(0.0) * 1e-9;
    auto correct = [&](double rate, bool& bad) {
      const double denom = 1.0 - rate * tau;
      if (denom <= 0.0) {
        bad = true;
        return rate;
      }
      return rate / denom;
    };
    const double rate = acc.span_s > 0.0 ? acc.total / acc.span_s : 0.0;
    mean = correct(rate, over_dead_time);
  } else {
    double sum = 0.0;
    for (double s : acc.samples) sum += s;
    mean = sum / static_cast<double>(n);
  }

  Value v = make_value(d, mean);
  v.n = static_cast<std::uint32_t>(n);
  if (n >= 2) {
    double mu = 0.0;
    for (double s : acc.samples) mu += s;
    mu /= static_cast<double>(n);
    double ss = 0.0;
    for (double s : acc.samples) ss += (s - mu) * (s - mu);
    double sigma = std::sqrt(ss / static_cast<double>(n - 1));
    if (is_counter(d.kind)) {
      const double tau = d.dead_time_ns.value_or(0.0) * 1e-9;
      // Propagate through the dead-time correction: d(r/(1-r tau))/dr = 1/(1-r tau)^2.
      const double denom = 1.0 - (acc.span_s > 0.0 ? acc.total / acc.span_s : 0.0) * tau;
      if (denom > 0.0) sigma /= denom * denom;
    }
    v.sigma = sigma * std::abs(d.software_gain);
  }
  if (over_dead_time) v.saturated = true;
  return v;
}

void AcquisitionEngine::ingest(std::size_t i, const Frame& frame, TimePoint now) {
  if (frame.ts < request_start_) {
    ++stale_frames_;
    return;
  }
  if (have_seq_[i]) {
    if (frame.seq <= last_seq_[i]) {
      ++stale_frames_;
      return;
    }
    if (frame.seq > last_seq_[i] + 1) dropped_frames_ += frame.seq - last_seq_[i] - 1;
  }
  have_seq_[i] = true;
  last_seq_[i] = frame.seq;
  last_frame_[i] = now;
  stalled_[i] = false;

  if (frame.integrated) {
    ingest_integrated(i, frame);
  } else {
    ingest_raw(i, frame);
  }
}

void AcquisitionEngine::ingest_integrated(std::size_t i, const Frame& frame) {
  if (frame.span > Duration::zero()) integration_ = frame.span;
  Partial p;
  p.acquirer = i;
  p.ts = frame.ts;
  p.integration = integration_;
  for (auto di : acq_dets_[i]) {
    const auto& d = detectors_[di];
    const auto v = frame.value(d.channel);
    p.values.emplace_back(di, v ? std::optional<Value>(make_value(d, *v)) : std::nullopt);
  }
  pending_.push_back(std::move(p));
}

void AcquisitionEngine::ingest_raw(std::size_t i, const Frame& frame) {
  const std::int64_t index = (frame.ts - epoch_).count() / integration_.count();
  Bin& bin = bins_[i];
  if (bin.open && index > bin.index) close_bin(i);
  if (!bin.open) {
    bin.open = true;
    bin.index = index;
    bin.acc.assign(acq_dets_[i].size(), Acc{});
  }
  const double span_s = to_seconds(frame.span);
  for (std::size_t k = 0; k < acq_dets_[i].size(); ++k) {
    const auto& d = detectors_[acq_dets_[i][k]];
    const auto v = frame.value(d.channel);
    if (!v) continue;
    Acc& acc = bin.acc[k];
    if (is_counter(d.kind)) {
      acc.total += *v;
      acc.span_s += span_s;
      acc.samples.push_back(span_s > 0.0 ? *v / span_s : *v);
    } else {
      acc.samples.push_back(*v);
    }
  }
}

void AcquisitionEngine::close_bin(std::size_t i) {
  Bin& bin = bins_[i];
  if (!bin.open) return;
  Partial p;
  p.acquirer = i;
  p.ts = bin_end(bin.index);
  p.integration = integration_;
  for (std::size_t k = 0; k < acq_dets_[i].size(); ++k) {
    p.values.emplace_back(acq_dets_[i][k], finish(detectors_[acq_dets_[i][k]], bin.acc[k]));
  }
  pending_.push_back(std::move(p));
  bin.open = false;
  bin.acc.clear();
}

void AcquisitionEngine::merge(TimePoint now, std::vector<Reading>& out) {
  constexpr auto npos = std::numeric_limits<std::size_t>::max();
  while (!pending_.empty()) {
    const auto first = std::min_element(pending_.begin(), pending_.end(),
                                        [](const Partial& a, const Partial& b) { return a.ts < b.ts; });
    const Duration half = integration_ / 2;
    std::vector<std::size_t> pick(acquirers_.size(), npos);
    pick[first->acquirer] = static_cast<std::size_t>(first - pending_.begin());
    for (std::size_t j = 0; j < pending_.size(); ++j) {
      const auto& p = pending_[j];
      if (pick[p.acquirer] == npos && abs_diff(p.ts, first->ts) <= half) pick[p.acquirer] = j;
    }
    const bool complete = std::none_of(pick.begin(), pick.end(), [&](auto v) { return v == npos; });
    // A lagging acquirer gets one integration period of grace, then the row
    // goes out with nullopt for its detectors.
    if (!complete && now - first->ts < integration_) break;

    Reading row;
    row.ts = first->ts;
    row.integration = integration_;
    for (const auto& d : detectors_) row.values[d.name] = std::nullopt;
    std::vector<std::size_t> used;
    for (auto j : pick) {
      if (j == npos) continue;
      for (const auto& [di, v] : pending_[j].values) row.values[detectors_[di].name] = v;
      used.push_back(j);
    }
    std::sort(used.rbegin(), used.rend());
    for (auto j : used) pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(j));
    out.push_back(std::move(row));
  }
}

void AcquisitionEngine::check_health(std::size_t i, TimePoint now, std::vector<Alarm>& alarms) {
  if (stalled_[i]) return;
  const Duration limit = scaled(integration_, options_.timeout_factor) + options_.timeout_slack;
  if (now - last_frame_[i] < limit) return;
  stalled_[i] = true;
  Alarm a;
  a.source = "acquisition";
  a.severity = AlarmSeverity::Warning;
  a.message = "no frame from acquirer " + std::to_string(i) + " within " +
              std::to_string(to_seconds(limit)) + " s";
  a.ts = now;
  alarms.push_back(std::move(a));
}

void AcquisitionEngine::deliver(std::vector<Reading>& readings, std::vector<Alarm>& alarms) {
  if (!alarms.empty()) {
    const Error err{ErrorKind::Timeout, alarms.front().message, "acquisition"};
    stream_->fail(err);
    {
      std::lock_guard lock(mutex_);
      if (collecting_) collect_error_ = err;
    }
    clock_.notify_all(collect_cv_);
  }
  if (!readings.empty()) {
    for (const auto& r : readings) stream_->push(r);
    {
      std::lock_guard lock(mutex_);
      if (collecting_) collected_.insert(collected_.end(), readings.begin(), readings.end());
    }
    clock_.notify_all(collect_cv_);
  }
  for (auto& r : readings) bus_.publish(IntensityReading{std::move(r)});
  for (const auto& a : alarms) bus_.publish(a);
}

void AcquisitionEngine::poll(std::size_t index) {
  if (index >= acquirers_.size()) return;
  std::vector<Reading> out;
  std::vector<Alarm> alarms;

  // Registered for the whole poll so stop() can wait for it.
  struct InFlight {
    AcquisitionEngine& engine;
    ~InFlight() {
      // Notify under the lock: the waiter may destroy the engine once it wakes.
      std::lock_guard lock(engine.mutex_);
      auto& ids = engine.polling_;
      ids.erase(std::find(ids.begin(), ids.end(), std::this_thread::get_id()));
      engine.clock_.notify_all(engine.polls_cv_);
    }
  };
  {
    std::lock_guard lock(mutex_);
    // A tick dispatched before stop() cancelled its job must not reach next().
    if (!running_) return;
    polling_.push_back(std::this_thread::get_id());
  }
  const InFlight in_flight{*this};

  for (int k = 0; k < kMaxDrainPerPoll; ++k) {
    auto frame = acquirers_[index]->next(options_.poll_timeout);
    if (!frame) {
      Alarm a;
      a.source = "acquisition";
      a.severity = AlarmSeverity::Warning;
      a.message = "acquirer " + std::to_string(index) + ": " + to_string(frame.error());
      a.ts = clock_.now();
      bus_.publish(a);
      break;
    }
    if (!*frame) break;
    std::lock_guard lock(mutex_);
    if (!running_ && !collecting_) return;
    ingest(index, **frame, clock_.now());
  }

  {
    std::lock_guard lock(mutex_);
    const TimePoint now = clock_.now();
    if (bins_[index].open && now >= bin_end(bins_[index].index)) close_bin(index);
    merge(now, out);
    check_health(index, now, alarms);
  }
  deliver(out, alarms);
}

Result<std::vector<Reading>> AcquisitionEngine::acquire(std::size_t n) {
  return acquire_impl(n, std::nullopt);
}

Result<std::vector<Reading>> AcquisitionEngine::acquire(Duration duration) {
  return acquire_impl(0, duration);
}

Result<std::vector<Reading>> AcquisitionEngine::acquire_impl(std::size_t n,
                                                              std::optional<Duration> duration) {
  if (!duration && n == 0) return std::vector<Reading>{};
  bool started_here = false;
  if (!running()) {
    if (auto r = start(options_.integration); !r) return fail(r.error());
    started_here = true;
  } else {
    begin_request();
  }
  const TimePoint deadline = clock_.now() + duration.value_or(Duration::zero());
  {
    std::lock_guard lock(mutex_);
    collecting_ = true;
    cancel_ = false;
    collected_.clear();
    collect_error_.reset();
  }

  std::optional<Error> failure;
  std::size_t seen = 0;
  for (;;) {
    for (auto* a : acquirers_) {
      if (auto r = a->trigger(); !r) {
        failure = r.error();
        break;
      }
    }
    if (failure) break;

    std::unique_lock lock(mutex_);
    while (collected_.size() <= seen && !cancel_ && !collect_error_ &&
           !(duration && clock_.now() >= deadline)) {
      if (duration) {
        clock_.wait_until(collect_cv_, lock, deadline);
      } else {
        clock_.wait(collect_cv_, lock);  // a reading, an error or cancel() ends it
      }
    }
    if (cancel_) {
      failure = Error{ErrorKind::Cancelled, "acquire cancelled", "acquisition"};
      break;
    }
    // Readings that arrived before an error or the deadline still count.
    seen = collected_.size();
    if (!duration && seen >= n) break;
    if (collect_error_) {
      failure = *collect_error_;
      break;
    }
    if (duration && clock_.now() >= deadline) break;
  }

  std::vector<Reading> result;
  {
    std::lock_guard lock(mutex_);
    collecting_ = false;
    result = std::move(collected_);
    collected_.clear();
  }
  if (started_here) stop();
  if (failure) return fail(*failure);
  if (!duration && result.size() > n) result.resize(n);
  return result;
}

}  // namespace pychron::spectrometer
