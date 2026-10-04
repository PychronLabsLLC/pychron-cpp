#include "entry_bridge.hpp"

#include <future>

#include "pychron/core/env.hpp"

namespace pychron::ui {

namespace ps = persistence;

Result<std::unique_ptr<EntryBridge>> EntryBridge::open(Options options) {
  std::unique_ptr<EntryBridge> bridge(new EntryBridge());
  std::promise<Result<ps::Actor>> opened;
  auto future = opened.get_future();
  bridge->worker_ = std::thread([b = bridge.get(), options = std::move(options), &opened]() mutable {
    b->loop(std::move(options), [&opened](Result<ps::Actor> r) { opened.set_value(std::move(r)); });
  });
  auto actor = future.get();
  if (!actor) return fail(actor.error());  // the worker has returned; the destructor joins it
  bridge->actor_ = *actor;
  return bridge;
}

EntryBridge::~EntryBridge() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  cv_.notify_one();
  if (worker_.joinable()) worker_.join();
}

void EntryBridge::post(Job job) {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) return;
    jobs_.push_back(std::move(job));
  }
  cv_.notify_one();
}

void EntryBridge::loop(Options options, std::function<void(Result<ps::Actor>)> opened) {
  auto store = ps::open_store(ps::StoreConfig{options.url, false});
  if (!store) {
    opened(fail(store.error()));
    return;
  }
  const std::string host = !options.host.empty() ? options.host : env_var("HOSTNAME").value_or("localhost");
  const std::string user = !options.user.empty() ? options.user : env_var("USER").value_or("pychron");
  auto client = (*store)->register_client({host, "reduction", std::nullopt, "pychron-ui"});
  if (!client) {
    opened(fail(client.error()));
    return;
  }
  auto u = (*store)->ensure_user(*client, user);
  if (!u) {
    opened(fail(u.error()));
    return;
  }
  const ps::Actor actor{*u, *client};
  opened(actor);
  for (;;) {
    Job job;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [&] { return stopping_ || !jobs_.empty(); });
      if (jobs_.empty()) return;  // stopping, and nothing left to run
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    job(**store, actor);
  }
}

}  // namespace pychron::ui
