#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <typeindex>
#include <utility>

namespace pychron {

// Thread-safe typed publish/subscribe.
//
// publish<E>() calls every current E subscriber synchronously on the
// publishing thread, outside the bus lock, so handlers may publish or
// (un)subscribe themselves. A handler removed while a publish is in flight on
// another thread may still receive that one event. Exceptions thrown by a
// handler are swallowed so they cannot unwind into scheduler/transport
// threads.
class SignalBus {
  struct Impl;

 public:
  // RAII handle; unsubscribes on destruction or reset(). May outlive the bus.
  class Subscription {
   public:
    Subscription() = default;
    ~Subscription();
    Subscription(Subscription&& other) noexcept;
    Subscription& operator=(Subscription&& other) noexcept;
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    void reset();
    bool active() const noexcept { return id_ != 0 && !bus_.expired(); }

   private:
    friend class SignalBus;
    Subscription(std::weak_ptr<Impl> bus, std::type_index type, std::uint64_t id)
        : bus_(std::move(bus)), type_(type), id_(id) {}

    std::weak_ptr<Impl> bus_;
    std::type_index type_ = typeid(void);
    std::uint64_t id_ = 0;
  };

  SignalBus();
  ~SignalBus();
  SignalBus(const SignalBus&) = delete;
  SignalBus& operator=(const SignalBus&) = delete;

  template <class E>
  [[nodiscard]] Subscription subscribe(std::function<void(const E&)> handler) {
    auto erased = [h = std::move(handler)](const void* ev) { h(*static_cast<const E*>(ev)); };
    return add(typeid(E), std::move(erased));
  }

  template <class E>
  void publish(const E& event) const {
    dispatch(typeid(E), &event);
  }

  template <class E>
  std::size_t subscriber_count() const {
    return count(typeid(E));
  }

 private:
  using Erased = std::function<void(const void*)>;

  Subscription add(std::type_index type, Erased handler);
  void dispatch(std::type_index type, const void* event) const;
  std::size_t count(std::type_index type) const;

  std::shared_ptr<Impl> impl_;
};

}  // namespace pychron
