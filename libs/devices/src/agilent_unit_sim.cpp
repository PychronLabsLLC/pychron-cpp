#include "pychron/devices/agilent_unit_sim.hpp"

#include <algorithm>
#include <cctype>

namespace pychron {

namespace ag = codec::agilent;

namespace {

std::string upper(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

AgilentUnitSim::AgilentUnitSim(Listener on_route, bool relays_start_closed) : on_route_(std::move(on_route)) {
  for (int slot = 1; slot <= 3; ++slot) {
    for (int ch = 1; ch <= 20; ++ch) {
      const auto channel = std::to_string(slot) + (ch < 10 ? "0" : "") + std::to_string(ch);
      fitted_.insert(channel);
      closed_[channel] = relays_start_closed;
    }
  }
}

void AgilentUnitSim::error_locked(int code, std::string message) {
  if (errors_.size() < 20) errors_.push_back({code, std::move(message)});
}

void AgilentUnitSim::push_error(int code, std::string message) {
  std::lock_guard lock(mutex_);
  error_locked(code, std::move(message));
}

void AgilentUnitSim::set_identity(std::string line) {
  std::lock_guard lock(mutex_);
  identity_ = std::move(line);
}

bool AgilentUnitSim::relay_closed(const std::string& channel) const {
  std::lock_guard lock(mutex_);
  auto it = closed_.find(channel);
  return it == closed_.end() || it->second;
}

std::size_t AgilentUnitSim::queued_errors() const {
  std::lock_guard lock(mutex_);
  return errors_.size();
}

Bytes AgilentUnitSim::respond(const Bytes& tx) {
  auto request = ag::decode_request(tx);
  std::unique_lock lock(mutex_);
  if (!request) {
    error_locked(-113, "Undefined header");
    return {};
  }
  const auto header = upper(request->header);
  if (header == "*IDN?") return ag::encode_line(identity_);
  if (header == "*CLS") {
    errors_.clear();
    return {};
  }
  if (header == "SYST:ERR?") {
    if (errors_.empty()) return ag::encode_error(0, "");
    auto e = errors_.front();
    errors_.pop_front();
    return ag::encode_error(e.code, e.message);
  }
  const bool route = header == "ROUT:OPEN" || header == "ROUT:CLOSE";
  const bool query = header == "ROUT:OPEN?" || header == "ROUT:CLOSE?";
  if (!route && !query) {
    error_locked(-113, "Undefined header");
    return {};
  }
  const auto channel = ag::single_channel(request->argument);
  if (!channel || !fitted_.contains(*channel)) {
    error_locked(-222, "Data out of range");
    return {};
  }
  const bool asks_closed = header.starts_with("ROUT:CLOSE");
  if (query) {
    const bool closed = closed_[*channel];
    return ag::encode_line(closed == asks_closed ? "1" : "0");
  }
  closed_[*channel] = asks_closed;
  auto listener = on_route_;
  lock.unlock();
  if (listener) listener(*channel, asks_closed);
  return {};
}

SimTransport::Hook AgilentUnitSim::hook() {
  return [this](const Bytes& tx) { return respond(tx); };
}

}  // namespace pychron
