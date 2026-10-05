#include "pychron/experiment/lab/session.hpp"

#include <map>
#include <string>
#include <utility>

#include "pychron/core/events.hpp"
#include "pychron/experiment/measurement/adapters.hpp"
#include "pychron/devices/extraction/interfaces.hpp"
#include "pychron/experiment/persist/persister.hpp"
#include "pychron/laser/laser_system.hpp"
#include "pychron/laser/tray_camera.hpp"
#include "pychron/scripting/script_host.hpp"
#include "pychron/sim/sim_system.hpp"
#include "pychron/systems/extraction_line.hpp"
#include "pychron/systems/spectrometer/scan_service.hpp"
#include "pychron/systems/switch_valve_service.hpp"

namespace pychron::experiment::lab {

namespace fs = std::filesystem;

// Declaration order is construction order; members refer only to earlier ones.
struct LabSession::Services {
  Services(const Lab& lab, const SessionHardware& hw, const fs::path& data,
           std::map<std::string, std::string, std::less<>>& problems)
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
    // One laser system per driver of the line that is an extraction device,
    // under the driver's name: what a queue's extract_device names.
    for (const auto& [name, driver] : hw.line.config().drivers) {
      auto* device = dynamic_cast<extraction::IExtractionDevice*>(hw.line.device(name));
      if (device == nullptr) continue;
      auto system = std::make_unique<laser::LaserSystem>(name, *device, lab.trays, *lab.calibrations, &lab.patterns);
      // Where holes were found before, and (if the lab gives the device a
      // camera) the means to find them again.
      system->set_corrections(*lab.corrections);
      // A recording is for looking (elctl laser look): it never moves a
      // stage. Any other camera must be one that can centre holes here.
      const laser::CameraConfig* camera = lab.cameras.find(name);
      if (camera != nullptr && camera->source != laser::CameraSource::Recorded) {
        const bool simulated = hw.simulated ? hw.simulated(name)
                                            : hw.line.sim() != nullptr && hw.line.sim()->chromium(name) != nullptr;
        Result<void> usable = laser::usable_for_autocenter(*camera, simulated);
        if (usable) {
          auto frames = laser::make_frame_source(*camera, lab.paths.dir, system->sight(), hw.line.clock());
          if (frames) usable = system->attach_camera(*camera, std::move(*frames), hw.line.clock());
          else usable = fail(frames.error());
        }
        if (!usable) problems.insert_or_assign(name, usable.error().what);
      }
      lasers.emplace(name, std::move(system));
    }
    s.devices = [this](std::string_view name) -> extraction::IExtractionDevice* {
      const auto it = lasers.find(name);
      return it == lasers.end() ? nullptr : it->second.get();
    };
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
  // By device name; they refer to the line's drivers and the lab's trays and
  // calibrations, all of which outlive the session.
  std::map<std::string, std::unique_ptr<laser::LaserSystem>, std::less<>> lasers;
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
  services_ = std::make_unique<Services>(lab_, hardware_, options_.data, device_problems_);
  notifier_ = std::make_unique<Notifier>(hardware_.line.bus(), lab_.notifications, options_.notify);
}

LabSession::~LabSession() {
  abort();
  if (thread_.joinable()) thread_.join();
}

std::vector<std::string> LabSession::problems() const {
  std::vector<std::string> out;
  for (const auto& [device, what] : device_problems_) out.push_back(what);
  return out;
}

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
    if (const auto it = device_problems_.find(device); it != device_problems_.end()) {
      return fail(ErrorKind::Config, it->second, "experiment");
    }
  }
  // The previous queue has ended but its thread may still be publishing
  // QueueEnded; joined without mutex_ so a subscriber may call back in.
  // Only the owner's thread touches thread_ (start and wait).
  if (thread_.joinable()) thread_.join();
  auto ctx = services_->ctx;
  ctx.services.instrument.mass_spectrometer = queue.mass_spectrometer;
  ctx.services.instrument.analyst = queue.username;
  {
    std::lock_guard lock(mutex_);
    executor_ = std::make_shared<executor::Executor>(std::move(ctx), options_.executor);
    running_ = true;
    result_.reset();
  }
  notifier_->set_queue(queue.name, queue.email);
  thread_ = std::thread([this, queue = std::move(queue), from_row]() mutable { run(std::move(queue), from_row); });
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
  if (thread_.joinable()) thread_.join();
  std::lock_guard lock(mutex_);
  return result_;
}

std::size_t LabSession::pending_saves() const { return services_->save.pending(); }

TimePoint LabSession::now() const { return hardware_.line.clock().now(); }

Result<std::size_t> LabSession::resume_row(const fs::path& data) {
  return executor::Executor::resume_row(data / "executor_state.json");
}

}  // namespace pychron::experiment::lab
