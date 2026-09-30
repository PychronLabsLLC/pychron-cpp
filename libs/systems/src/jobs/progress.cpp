#include "pychron/systems/jobs/progress.hpp"

#include <algorithm>

#include "pychron/systems/jobs/cancel.hpp"

namespace pychron::jobs {

// ---- CancelToken ------------------------------------------------------------

void CancelToken::cancel() {
  cancelled_.store(true);
  // Run the hook every time: a blocking step may have reset its own cancel
  // flag after an earlier call.
  std::lock_guard lock(mutex_);
  if (hook_) hook_();
}

CancelToken::HookGuard CancelToken::on_cancel(Hook hook) {
  std::lock_guard lock(mutex_);
  hook_ = std::move(hook);
  if (cancelled_.load() && hook_) hook_();
  return HookGuard(this);
}

CancelToken::HookGuard::~HookGuard() {
  if (!token_) return;
  std::lock_guard lock(token_->mutex_);
  token_->hook_ = nullptr;
}

// ---- Progress ---------------------------------------------------------------

double ProgressUpdate::fraction() const noexcept {
  if (total == 0) return 0.0;
  return std::min(1.0, static_cast<double>(done) / static_cast<double>(total));
}

void Progress::report(ProgressUpdate update) {
  {
    std::lock_guard lock(mutex_);
    last_ = update;
  }
  if (sink_) sink_(update);
}

ProgressUpdate Progress::last() const {
  std::lock_guard lock(mutex_);
  return last_;
}

}  // namespace pychron::jobs
