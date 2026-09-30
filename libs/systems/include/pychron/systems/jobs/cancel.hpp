#pragma once

// Cooperative cancellation for tuning jobs (spectrometer spec section 6).
// Jobs poll cancelled() between steps; a blocking step (an acquisition) may
// install a hook that cancel() calls so it returns promptly.

#include <atomic>
#include <functional>
#include <mutex>

namespace pychron::jobs {

class CancelToken {
 public:
  using Hook = std::function<void()>;

  // Sets the flag, then runs the installed hook (if any). Idempotent.
  void cancel();
  bool cancelled() const noexcept { return cancelled_.load(); }

  // Installs `hook` for the lifetime of the returned guard. Runs it at once
  // if the token is already cancelled.
  class HookGuard {
   public:
    HookGuard() = default;
    HookGuard(HookGuard&& other) noexcept : token_(other.token_) { other.token_ = nullptr; }
    HookGuard& operator=(HookGuard&&) = delete;
    HookGuard(const HookGuard&) = delete;
    HookGuard& operator=(const HookGuard&) = delete;
    ~HookGuard();

   private:
    friend class CancelToken;
    explicit HookGuard(CancelToken* token) : token_(token) {}
    CancelToken* token_ = nullptr;
  };
  [[nodiscard]] HookGuard on_cancel(Hook hook);

 private:
  std::atomic<bool> cancelled_{false};
  std::mutex mutex_;
  Hook hook_;
};

}  // namespace pychron::jobs
