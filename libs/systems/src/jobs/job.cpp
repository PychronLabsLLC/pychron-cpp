#include "pychron/systems/jobs/job.hpp"

#include <memory>

namespace pychron::jobs {

std::string_view to_string(JobState state) noexcept {
  switch (state) {
    case JobState::Queued:
      return "queued";
    case JobState::Running:
      return "running";
    case JobState::Succeeded:
      return "succeeded";
    case JobState::Failed:
      return "failed";
    case JobState::Cancelled:
      return "cancelled";
  }
  return "unknown";
}

bool is_finished(JobState state) noexcept {
  return state == JobState::Succeeded || state == JobState::Failed || state == JobState::Cancelled;
}

JobSpec sweep_job(SweepSpec spec, Sweep::Options options) {
  return JobSpec{"sweep", [spec = std::move(spec), options = std::move(options)](JobContext& ctx) -> Result<std::any> {
                   Sweep sweep(options);
                   auto points = sweep.run(ctx.spectrometer, spec, ctx.progress, ctx.cancel);
                   if (!points) return fail(points.error());
                   return std::any(std::move(*points));
                 }};
}

}  // namespace pychron::jobs
