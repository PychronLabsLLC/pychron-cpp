#include "pychron/devices/pychron_valve_server_sim.hpp"

#include "pychron/codecs/pychron_tx.hpp"

namespace pychron {

namespace tx = codec::pychron_tx;

PychronValveServerSim::PychronValveServerSim(Listener on_change) : on_change_(std::move(on_change)) {}

void PychronValveServerSim::declare(std::set<std::string> names) {
  std::lock_guard lock(mutex_);
  declared_ = std::move(names);
}

void PychronValveServerSim::lock(const std::string& name) {
  std::lock_guard lock(mutex_);
  locked_.insert(name);
}

bool PychronValveServerSim::is_open(const std::string& name) const {
  std::lock_guard lock(mutex_);
  auto it = open_.find(name);
  return it != open_.end() && it->second;
}

Bytes PychronValveServerSim::respond(const Bytes& bytes) {
  auto request = tx::decode_request(bytes);
  if (!request) return tx::encode_error("003", "invalid command: ");
  std::unique_lock lock(mutex_);
  const auto& name = request->name;
  const bool known = declared_.empty() ? !name.empty() : declared_.contains(name);
  const bool actuate = request->verb == "Open" || request->verb == "Close";
  if (!actuate && request->verb != "GetValveState") return tx::encode_error("003", "invalid command: " + request->verb);
  if (!known) return tx::encode_error("005", name + " is not a registered valve name");
  if (!actuate) return tx::encode_state(open_[name]);
  if (locked_.contains(name)) return tx::encode_error("014", "Valve " + name + " is software locked");
  const bool want = request->verb == "Open";
  const bool changed = open_[name] != want;
  open_[name] = want;
  auto listener = on_change_;
  lock.unlock();
  if (changed && listener) listener(name, want);
  return tx::encode_ok(changed);
}

SimTransport::Hook PychronValveServerSim::hook() {
  return [this](const Bytes& tx) { return respond(tx); };
}

}  // namespace pychron
