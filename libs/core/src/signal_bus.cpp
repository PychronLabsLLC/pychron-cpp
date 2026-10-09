#include "pychron/core/signal_bus.hpp"

#include <mutex>
#include <unordered_map>
#include <vector>

namespace pychron {

struct SignalBus::Impl {
  struct Entry {
    std::uint64_t id;
    Erased handler;
  };
  using List = std::vector<Entry>;

  // Copy-on-write lists: publishers take a snapshot and call it unlocked.
  mutable std::mutex mutex;
  std::unordered_map<std::type_index, std::shared_ptr<const List>> handlers;
  std::uint64_t next_id = 1;

  void remove(std::type_index type, std::uint64_t id) {
    std::lock_guard lock(mutex);
    auto it = handlers.find(type);
    if (it == handlers.end()) return;
    auto next = std::make_shared<List>();
    next->reserve(it->second->size());
    for (const auto& e : *it->second) {
      if (e.id != id) next->push_back(e);
    }
    if (next->empty()) {
      handlers.erase(it);
    } else {
      it->second = std::move(next);
    }
  }
};

SignalBus::SignalBus() : impl_(std::make_shared<Impl>()) {}
SignalBus::~SignalBus() = default;

SignalBus::Subscription SignalBus::add(std::type_index type, Erased handler) {
  std::lock_guard lock(impl_->mutex);
  const auto id = impl_->next_id++;
  auto next = std::make_shared<Impl::List>();
  if (auto it = impl_->handlers.find(type); it != impl_->handlers.end()) *next = *it->second;
  next->push_back({id, std::move(handler)});
  impl_->handlers[type] = std::move(next);
  return Subscription(impl_, type, id);
}

void SignalBus::dispatch(std::type_index type, const void* event) const {
  std::shared_ptr<const Impl::List> snapshot;
  {
    std::lock_guard lock(impl_->mutex);
    auto it = impl_->handlers.find(type);
    if (it == impl_->handlers.end()) return;
    snapshot = it->second;
  }
  for (const auto& e : *snapshot) {
    try {
      e.handler(event);
    } catch (...) {  // NOLINT(bugprone-empty-catch): see below
      // A faulty subscriber must not take down the publishing thread.
    }
  }
}

std::size_t SignalBus::count(std::type_index type) const {
  std::lock_guard lock(impl_->mutex);
  auto it = impl_->handlers.find(type);
  return it == impl_->handlers.end() ? 0 : it->second->size();
}

SignalBus::Subscription::~Subscription() { reset(); }

SignalBus::Subscription::Subscription(Subscription&& other) noexcept
    : bus_(std::move(other.bus_)), type_(other.type_), id_(std::exchange(other.id_, 0)) {}

SignalBus::Subscription& SignalBus::Subscription::operator=(Subscription&& other) noexcept {
  if (this != &other) {
    reset();
    bus_ = std::move(other.bus_);
    type_ = other.type_;
    id_ = std::exchange(other.id_, 0);
  }
  return *this;
}

void SignalBus::Subscription::reset() {
  if (id_ == 0) return;
  if (auto bus = bus_.lock()) bus->remove(type_, id_);
  bus_.reset();
  id_ = 0;
}

}  // namespace pychron
