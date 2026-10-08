#include "pychron/experiment/lab/session.hpp"

#include <chrono>
#include <map>
#include <string>
#include <utility>

#include "pychron/core/events.hpp"
#include "pychron/experiment/measurement/adapters.hpp"
#include "pychron/devices/extraction/interfaces.hpp"
#include "pychron/experiment/persist/persister.hpp"
#include "pychron/experiment/lab/lasers.hpp"
#include "pychron/scripting/script_host.hpp"
#include "pychron/sim/sim_system.hpp"
#include "pychron/systems/extraction_line.hpp"
#include "pychron/systems/spectrometer/scan_service.hpp"
#include "pychron/systems/line_cryo_service.hpp"
#include "pychron/systems/line_pressure_service.hpp"
#include "pychron/systems/switch_valve_service.hpp"

namespace pychron::experiment::lab {

namespace fs = std::filesystem;

struct LabSession::QueueLease {
  Lasers::Lease lease;
};

// Declaration order is construction order; members refer only to earlier ones.
struct LabSession::Services {
  Services(const Lab& lab, const SessionHardware& hw, const fs::path& data, Lasers& lasers)
      : host(scripting::make_script_host()),
        script_valves(hw.line.switches(), "script"),
        script_pressure(hw.line),
        script_cryo(hw.line),
        valves(hw.line, "measurement"),
        instrument(hw.spectrometer, &hw.line),
        files(data / "records"),
        spool(data / "spool"),
        save(spool, files),
        aliquots(files) {
    if (hw.spectrometer != nullptr) {
      port.emplace(*hw.spectrometer);
      jobs::PeakCenterOptions pc;
      pc.bus = &hw.line.bus();  // PeakCenterDone for the UI
      peak_center.emplace(*hw.spectrometer, lab.peak_centers, nullptr, pc);
    }
    auto& s = ctx.services;
    s.clock = &hw.line.clock();
    s.bus = &hw.line.bus();
    s.scripts = host.get();
    s.resolver = &lab.scripts->resolver();
    s.line.valves = &script_valves;
    s.line.pressure = &script_pressure;
    if (hw.line.config().cryo) s.line.cryo = &script_cryo;
    s.devices = [&lasers](std::string_view name) -> extraction::IExtractionDevice* { return lasers.find(name); };
    s.spectrometer = port ? &*port : nullptr;
    s.valves = &valves;
    s.peak_center = peak_center ? &*peak_center : nullptr;
    s.instrument_metrics = &instrument;
    if (auto* spec = hw.spectrometer) {
      s.spectrometer_info = [spec] {
        const auto st = spec->snapshot();
        return run::SpectrometerInfo{st.hash_hex(), st.field_table,
                                     std::chrono::duration<double>(st.integration).count()};
      };
    }
    s.plans = lab.plans.get();
    s.conditionals = lab.conditionals.get();
    s.aliquots = &aliquots;
    s.persister = &files;
    s.save = &save;
    ctx.pre_run_metrics = &instrument;
    ctx.blank = default_blank_factory(lab.ids);
  }

  std::unique_ptr<scripting::IScriptHost> host;
  systems::SwitchValveService script_valves;
  systems::LinePressureService script_pressure;
  systems::LineCryoService script_cryo;
  std::optional<measurement::SpectrometerPort> port;
  measurement::ExtractionLineValves valves;
  measurement::InstrumentMetrics instrument;
  std::optional<measurement::SpectrometerPeakCenter> peak_center;
  persist::FilePersister files;
  persist::Spool spool;
  persist::SavePipeline save;
  persist::AliquotAllocator aliquots;
  executor::ExecutorContext ctx;
};

LabSession::LabSession(const Lab& lab, SessionHardware hardware, SessionOptions options)
    : lab_(lab), hardware_(hardware), options_(std::move(options)) {
  if (options_.executor.state_file.empty()) options_.executor.state_file = options_.data / "executor_state.json";
  if (hardware_.lasers == nullptr) own_lasers_ = std::make_unique<Lasers>(lab_, hardware_.line, hardware_.simulated);
  lasers_ = hardware_.lasers != nullptr ? hardware_.lasers : own_lasers_.get();
  services_ = std::make_unique<Services>(lab_, hardware_, options_.data, *lasers_);
  notifier_ = std::make_unique<Notifier>(hardware_.line.bus(), lab_.notifications, options_.notify);
}

LabSession::~LabSession() {
  abort();
  join();
}

// The queue thread may still have to wait in the line's clock, so it is not
// joined until it has said it is done; that is waited for through the clock,
// with mutex_ released, so a subscriber may call back in meanwhile.
void LabSession::join() {
  if (!thread_.joinable()) return;
  {
    std::unique_lock lock(mutex_);
    while (!thread_done_) hardware_.line.clock().wait(thread_done_cv_, lock);
  }
  thread_.join();
}

std::vector<std::string> LabSession::problems() const { return lasers_->problems(); }

bool LabSession::has_spectrometer() const noexcept { return hardware_.spectrometer != nullptr; }

Result<void> LabSession::start(QueueSpec queue, std::size_t from_row) {
  if (running()) return fail(ErrorKind::Config, "a queue is already running", "experiment");
  if (auto ok = check(queue); !ok) return ok;
  // A device whose camera cannot be used as the lab asks is not run without
  // it: the queue waits for the lab to be put right.
  for (const auto& r : queue.runs) {
    if (r.skip) continue;
    ExtractionSpec nothing;
    nothing.device = r.extraction.device;
    if (r.extraction == nothing) continue;  // the run extracts nothing
    const std::string& device = r.extraction.device.empty() ? queue.extract_device : r.extraction.device;
    if (const std::string* problem = lasers_->problem_of(device)) {
      return fail(ErrorKind::Config, *problem, "experiment");
    }
  }
  // A stop somebody pressed is put right by somebody, not by the next queue.
  if (const auto stopped = lasers_->stopped(); !stopped.empty()) {
    return fail(ErrorKind::Config, stopped.front() + ": emergency stop not reset", "experiment");
  }
  // The lasers are the queue's from here until it ends.
  auto lease = lasers_->drive(Lasers::Driver::Queue);
  if (!lease) return fail(ErrorKind::Config, lease.error().what, "experiment");
  // A beam somebody left on by hand is nobody's now, and under a queue the
  // window could no longer close it: it is closed first, by whoever opened it.
  if (const auto firing = lasers_->firing(); !firing.empty()) {
    return fail(ErrorKind::Config, firing.front() + ": the laser is firing; stop it before starting a queue",
                "experiment");
  }
  // The previous queue has ended but its thread may still be publishing
  // QueueEnded; joined without mutex_ so a subscriber may call back in.
  // Only the owner's thread touches thread_ (start and wait).
  join();
  auto ctx = services_->ctx;
  ctx.services.instrument.mass_spectrometer = queue.mass_spectrometer;
  ctx.services.instrument.analyst = queue.username;
  {
    std::lock_guard lock(mutex_);
    executor_ = std::make_shared<executor::Executor>(std::move(ctx), options_.executor);
    running_ = true;
    thread_done_ = false;
    result_.reset();
    lease_ = std::make_unique<QueueLease>(QueueLease{std::move(*lease)});
  }
  notifier_->set_queue(queue.name, queue.email);
  // Time does not move on between the thread's start and its Participant.
  const Clock& clock = hardware_.line.clock();
  auto hold = std::make_shared<Clock::Hold>(clock);
  thread_ = std::thread([this, &clock, hold, queue = std::move(queue), from_row]() mutable {
    Clock::Participant participant(clock, "lab.session");
    hold.reset();
    run(std::move(queue), from_row);
    // Said while this thread is still a participant.
    std::lock_guard lock(mutex_);
    thread_done_ = true;
    clock.notify_all(thread_done_cv_);
  });
  return {};
}

Result<void> LabSession::check(const QueueSpec& queue) const {
  const auto c = check_lab_queue(lab_, queue);
  if (c.ok()) return {};
  int errors = 0;
  std::string first;
  for (const auto& d : c.all()) {
    if (d.severity != Severity::Error) continue;
    if (errors++ == 0) first = describe(d);
  }
  return fail(ErrorKind::Config, "the queue has " + std::to_string(errors) + " error(s); first: " + first,
              "experiment");
}

Result<std::uint64_t> LabSession::edit(std::uint64_t base, const QueueSpec& queue) {
  auto ex = active();
  if (!ex) return fail(ErrorKind::Config, "no queue is running", "experiment");
  if (auto ok = check(queue); !ok) return fail(ok.error());
  return ex->edit(base, queue.runs, "edited by the operator");
}

void LabSession::run(QueueSpec spec, std::size_t from_row) {
  auto* scan = hardware_.scan;
  const bool paused_here = scan != nullptr && scan->running() && !scan->paused();
  if (paused_here) scan->pause();

  hardware_.line.bus().publish(QueueStarted{spec.runs.size(), from_row});
  ExperimentQueue queue(std::move(spec));
  std::shared_ptr<executor::Executor> ex;
  {
    std::lock_guard lock(mutex_);
    ex = executor_;
  }
  auto result = ex->execute(queue, from_row);

  auto& bus = hardware_.line.bus();
  if (paused_here) {
    if (auto r = scan->resume(); !r) {
      bus.publish(Log{LogLevel::Warn, "experiment", "scan did not resume: " + to_string(r.error()),
                      hardware_.line.clock().now()});
    }
  }
  {
    std::lock_guard lock(mutex_);
    result_ = result;
    running_ = false;
    lease_.reset();
  }
  notifier_->queue_ended(result);
  bus.publish(QueueEnded{std::move(result)});
}

void LabSession::notify_test() { notifier_->send_test(lab_.paths.dir.string()); }

void LabSession::stop() {
  if (auto ex = active()) ex->stop();
}

void LabSession::cancel() {
  if (auto ex = active()) ex->cancel();
}

void LabSession::abort() {
  if (auto ex = active()) ex->abort();
}

void LabSession::truncate(bool quick) {
  if (auto ex = active()) ex->truncate(quick);
}

std::shared_ptr<executor::Executor> LabSession::active() const {
  std::lock_guard lock(mutex_);
  return running_ ? executor_ : nullptr;
}

bool LabSession::running() const {
  std::lock_guard lock(mutex_);
  return running_;
}

executor::ExecutorState LabSession::state() const {
  std::shared_ptr<executor::Executor> ex;
  {
    std::lock_guard lock(mutex_);
    ex = executor_;
  }
  return ex ? ex->state() : executor::ExecutorState::Idle;
}

std::optional<executor::QueueResult> LabSession::wait() {
  join();
  std::lock_guard lock(mutex_);
  return result_;
}

std::size_t LabSession::pending_saves() const { return services_->save.pending(); }

TimePoint LabSession::now() const { return hardware_.line.clock().now(); }

Result<std::size_t> LabSession::resume_row(const fs::path& data) {
  return executor::Executor::resume_row(data / "executor_state.json");
}

}  // namespace pychron::experiment::lab
