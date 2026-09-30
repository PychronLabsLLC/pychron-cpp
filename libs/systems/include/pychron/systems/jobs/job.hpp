#pragma once

// Tuning job model (spectrometer spec section 6). A JobSpec is a kind name
// plus a body: a thin runner around a pure algorithm. UI, CLI and the future
// experiment engine submit the same JobSpecs to a JobRunner, which is the
// only path by which tuning touches hardware.

#include <any>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/systems/jobs/cancel.hpp"
#include "pychron/systems/jobs/progress.hpp"
#include "pychron/systems/jobs/sweep.hpp"
#include "pychron/systems/spectrometer/spectrometer.hpp"

namespace pychron::jobs {

using JobId = std::uint64_t;
using spectrometer::SpectrometerState;

enum class JobState { Queued, Running, Succeeded, Failed, Cancelled };

// "queued", "running", "succeeded", "failed", "cancelled".
std::string_view to_string(JobState state) noexcept;
bool is_finished(JobState state) noexcept;

struct JobContext {
  JobId id = 0;
  Spectrometer& spectrometer;
  Progress& progress;
  CancelToken& cancel;
};

// Result type is up to the job kind (e.g. std::vector<SweepPoint> for
// "sweep"); consumers any_cast on `kind`.
using JobBody = std::function<Result<std::any>(JobContext&)>;

struct JobSpec {
  std::string kind;
  JobBody body;
};

struct Job {
  JobId id = 0;
  std::string kind;
  JobSpec spec;
  JobState state = JobState::Queued;
  ProgressUpdate progress;
  std::any result;                  // set when Succeeded
  std::optional<Error> error;       // set when Failed or Cancelled
  std::optional<SpectrometerState> before, after;  // absent if the body never ran
  TimePoint submitted{}, started{}, finished{};
};

// ---- SignalBus events -----------------------------------------------------

struct JobStarted {
  JobId id = 0;
  std::string kind;
  SpectrometerState before;
};

struct JobProgress {
  JobId id = 0;
  std::string kind;
  ProgressUpdate update;
};

// The final record; persistence subscribes to this.
struct JobFinished {
  Job job;
};

// ---- built-in kinds ---------------------------------------------------------

// Kind "sweep": runs Sweep; result is std::vector<SweepPoint>.
JobSpec sweep_job(SweepSpec spec, Sweep::Options options = {});

}  // namespace pychron::jobs
