#include "follow_mailbox.hpp"

#include <utility>

namespace pychron::ui {

bool FollowMailbox::add(const laser::FollowSample& sample) {
  const std::lock_guard lock(mutex_);
  pending_.push_back(sample);
  if (pending_.size() > kMaxPending) {
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(pending_.size() - kMaxPending));
  }
  return !std::exchange(posted_, true);
}

std::vector<laser::FollowSample> FollowMailbox::take() {
  const std::lock_guard lock(mutex_);
  posted_ = false;
  return std::exchange(pending_, {});
}

}  // namespace pychron::ui
