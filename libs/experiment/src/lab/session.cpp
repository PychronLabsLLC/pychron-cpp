#include "pychron/experiment/lab/session.hpp"

#include <utility>

#include "pychron/core/events.hpp"
#include "pychron/experiment/measurement/adapters.hpp"
#include "pychron/experiment/persist/persister.hpp"
#include "pychron/scripting/script_host.hpp"
#include "pychron/systems/extraction_line.hpp"
#include "pychron/systems/spectrometer/scan_service.hpp"
#include "pychron/systems/switch_valve_service.hpp"

namespace pychron::experiment::lab {

namespace fs = std::filesystem;

// Declaration order is construction order; members refer only to earlier ones.
struct LabSession::Services {
  Services(const Lab& lab, const SessionHardware& hw, const fs::path& data)
      : host(scripting::make_script_host()),
        script_valves(hw.line.switches(), "script"),
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
  services_ = std::make_unique<Services>(lab_, hardware_, options_.data);
}

LabSession::~LabSession() {
  abort();
  if (thread_.joinable()) thread_.join();
}

bool LabSession::has_spectrometer() const noexcept { return hardware_.spectrometer != nullptr; }

Result<void> LabSession::start(QueueSpec queue, std::size_t from_row) {
  std::lock_guard lock(mutex_);
  if (running_) return fail(ErrorKind::Config, "a queue is already running", "experiment");
  const auto check = check_lab_queue(lab_, queue);
  if (!check.ok()) {
    int errors = 0;
    std::string first;
    for (const auto& d : check.all()) {
      if (d.severity != Severity::Error) continue;
      if (errors++ == 0) first = describe(d);
    }
    return fail(ErrorKind::Config,
                "the queue has " + std::to_string(errors) + " error(s); first: " + first, "experiment");
  }
  if (thread_.joinable()) thread_.join();  // the previous queue has ended
  auto ctx = services_->ctx;
  ctx.services.instrument.mass_spectrometer = queue.mass_spectrometer;
  ctx.services.instrument.analyst = queue.username;
  executor_ = std::make_shared<executor::Executor>(std::move(ctx), options_.executor);
  running_ = true;
  result_.reset();
  thread_ = std::thread([this, queue = std::move(queue), from_row]() mutable { run(std::move(queue), from_row); });
  return {};
}

void LabSession::run(QueueSpec spec, std::size_t from_row) {
  auto* scan = hardware_.scan;
  const bool paused_here = scan != nullptr && scan->running() && !scan->paused();
  if (paused_here) scan->pause();

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
  }
  bus.publish(QueueEnded{std::move(result)});
}

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
  if (thread_.joinable()) thread_.join();
  std::lock_guard lock(mutex_);
  return result_;
}

std::size_t LabSession::pending_saves() const { return services_->save.pending(); }

Result<std::size_t> LabSession::resume_row(const fs::path& data) {
  return executor::Executor::resume_row(data / "executor_state.json");
}

}  // namespace pychron::experiment::lab
