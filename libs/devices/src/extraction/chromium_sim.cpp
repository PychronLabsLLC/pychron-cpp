#include "pychron/devices/extraction/chromium_sim.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <optional>

namespace pychron::extraction {

namespace {

constexpr double kScanMoveSpeed = 5000;  // microns per second

std::string lower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::string_view trim(std::string_view s) {
  auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!s.empty() && blank(s.front())) s.remove_prefix(1);
  while (!s.empty() && blank(s.back())) s.remove_suffix(1);
  return s;
}

// Not std::from_chars: its floating-point overloads are missing from older libc++.
std::optional<double> number(std::string_view s) { return codec::parse_decimal(trim(s)); }

// Every comma-separated value, or nullopt if any is not a number.
std::optional<std::vector<double>> numbers(std::string_view s) {
  std::vector<double> out;
  if (trim(s).empty()) return out;
  while (true) {
    const auto at = s.find(',');
    const auto v = number(s.substr(0, at));
    if (!v) return std::nullopt;
    out.push_back(*v);
    if (at == std::string_view::npos) break;
    s.remove_prefix(at + 1);
  }
  return out;
}

std::string shortest(double v) {
  std::array<char, 32> buffer{};
  const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), v);
  return ec == std::errc{} ? std::string(buffer.data(), end) : std::string("0");
}

std::string error(int code) { return "?" + std::to_string(code) + "\r"; }
std::string line(std::string_view text) { return std::string(text) + "\r"; }
const std::string kSilent;

}  // namespace

ChromiumSim::ChromiumSim(const Clock& clock) : clock_(clock), advanced_(clock.now()) {}

SimTransport::Hook ChromiumSim::hook() {
  return [this](const Bytes& tx) {
    // One command per line; a write may carry several.
    std::string reply;
    std::string_view rest_view;
    const std::string text = to_string(tx);
    rest_view = text;
    while (!rest_view.empty()) {
      const auto nl = rest_view.find('\n');
      const std::string_view command = trim(rest_view.substr(0, nl));
      if (!command.empty()) {
        std::lock_guard lock(mutex_);
        advance();
        log_.emplace_back(command);
        reply += handle(command);
      }
      if (nl == std::string_view::npos) break;
      rest_view.remove_prefix(nl + 1);
    }
    return to_bytes(reply);
  };
}

void ChromiumSim::trip_interlock(std::string name) {
  std::lock_guard lock(mutex_);
  interlocks_.push_back(std::move(name));
  firing_ = false;  // an interlock shuts the beam
}

void ChromiumSim::clear_interlocks() {
  std::lock_guard lock(mutex_);
  interlocks_.clear();
}

void ChromiumSim::add_scan(Microns start) {
  std::lock_guard lock(mutex_);
  scans_.push_back(start);
}

void ChromiumSim::put_on_limit(char axis, int side) {
  std::lock_guard lock(mutex_);
  const int i = axis == 'x' ? 0 : axis == 'y' ? 1 : 2;
  limits_[static_cast<std::size_t>(i)] = side;
}

void ChromiumSim::set_id(std::string id) {
  std::lock_guard lock(mutex_);
  id_ = std::move(id);
}

void ChromiumSim::fail_next(std::string command_prefix, int code) {
  std::lock_guard lock(mutex_);
  refusals_.push_back({lower(command_prefix), code, Fault::Refuse, 0, {}});
}

void ChromiumSim::swallow_next(std::string command_prefix, int skip) {
  std::lock_guard lock(mutex_);
  refusals_.push_back({lower(command_prefix), 0, Fault::Swallow, skip, {}});
}

void ChromiumSim::silence_next(std::string command_prefix, int skip) {
  std::lock_guard lock(mutex_);
  refusals_.push_back({lower(command_prefix), 0, Fault::Silence, skip, {}});
}

void ChromiumSim::trip_on_next(std::string command_prefix, std::string interlock) {
  std::lock_guard lock(mutex_);
  refusals_.push_back({lower(command_prefix), 0, Fault::Trip, 0, std::move(interlock)});
}

bool ChromiumSim::enabled() const {
  std::lock_guard lock(mutex_);
  return enabled_;
}

bool ChromiumSim::firing() const {
  std::lock_guard lock(mutex_);
  return firing_;
}

double ChromiumSim::output() const {
  std::lock_guard lock(mutex_);
  return output_;
}

ChromiumSim::Microns ChromiumSim::position() const {
  std::lock_guard lock(mutex_);
  const_cast<ChromiumSim*>(this)->advance();
  return {std::llround(at_[0]), std::llround(at_[1]), std::llround(at_[2])};
}

std::vector<std::string> ChromiumSim::log() const {
  std::lock_guard lock(mutex_);
  return log_;
}

void ChromiumSim::advance() {
  const TimePoint now = clock_.now();
  const double dt = std::chrono::duration<double>(now - advanced_).count();
  advanced_ = now;
  if (dt <= 0) return;
  for (std::size_t i = 0; i < 3; ++i) {
    if (speed_[i] <= 0) continue;
    const double left = target_[i] - at_[i];
    const double step = speed_[i] * dt;
    if (std::abs(left) <= step) {
      at_[i] = target_[i];  // exactly on target
      speed_[i] = 0;
    } else {
      at_[i] += left > 0 ? step : -step;
    }
  }
}

void ChromiumSim::start_move(const std::array<double, 3>& target, const std::array<double, 3>& speed) {
  for (std::size_t i = 0; i < 3; ++i) {
    // A speed of 0 leaves that axis where it is.
    target_[i] = speed[i] > 0 ? target[i] : at_[i];
    speed_[i] = speed[i] > 0 && target[i] != at_[i] ? speed[i] : 0;
  }
}

bool ChromiumSim::at_rest() const {
  return speed_[0] == 0 && speed_[1] == 0 && speed_[2] == 0;
}

std::string ChromiumSim::handle(std::string_view command) {
  const std::string lowered = lower(command);
  bool silenced = false;
  for (auto it = refusals_.begin(); it != refusals_.end(); ++it) {
    if (!lowered.starts_with(it->prefix)) continue;
    if (it->skip > 0) {
      --it->skip;
      continue;
    }
    const Refusal fault = *it;
    refusals_.erase(it);
    switch (fault.fault) {
      case Fault::Refuse: return error(fault.code);
      case Fault::Swallow: return kSilent;
      case Fault::Trip:
        interlocks_.push_back(fault.interlock);
        firing_ = false;
        return kSilent;
      case Fault::Silence: silenced = true; break;
    }
    break;
  }
  if (silenced) {
    (void)dispatch(lowered);
    return kSilent;
  }
  return dispatch(lowered);
}

std::string ChromiumSim::dispatch(const std::string& lowered) {
  const auto dot = lowered.find('.');
  if (dot == std::string::npos) return error(1);
  const std::string_view component = std::string_view(lowered).substr(0, dot);
  std::string_view rest = std::string_view(lowered).substr(dot + 1);
  const auto space = rest.find(' ');
  const std::string_view name = rest.substr(0, space);
  const std::string_view args = space == std::string_view::npos ? std::string_view{} : trim(rest.substr(space + 1));

  if (component == "sys") {
    if (name == "id?") return line(id_);
    return error(2);
  }
  if (component == "laser") return laser(name, args);
  if (component == "stage") return stage(name, args);
  if (component == "scans") return scans(name, args);
  return error(1);
}

std::string ChromiumSim::laser(std::string_view name, std::string_view args) {
  if (name == "status?") return line(std::to_string(interlocks_.size()));
  if (name == "interlocks?") {
    std::string joined;
    for (const auto& i : interlocks_) joined += (joined.empty() ? "" : ",") + i;
    return line(joined);
  }
  if (name == "enable?") return line(enabled_ ? "1" : "0");
  if (name == "output?") return line(shortest(output_));
  if (name == "enable") {
    const auto v = number(args);
    if (!v) return error(3);
    enabled_ = *v != 0;
    if (!enabled_) firing_ = false;
    return kSilent;
  }
  if (name == "output") {
    const auto v = number(args);
    if (!v || *v < 0 || *v > 100) return error(3);
    output_ = *v;
    return kSilent;
  }
  if (name == "fire") {
    if (!enabled_ || !interlocks_.empty()) return error(4);
    firing_ = true;
    return kSilent;
  }
  if (name == "stop") {
    firing_ = false;
    return kSilent;
  }
  return error(2);
}

std::string ChromiumSim::stage(std::string_view name, std::string_view args) {
  if (name == "pos?") {
    return line(std::to_string(std::llround(at_[0])) + "," + std::to_string(std::llround(at_[1])) + "," +
                std::to_string(std::llround(at_[2])));
  }
  if (name == "status?") {
    return line(std::to_string(limits_[0]) + "," + std::to_string(limits_[1]) + "," + std::to_string(limits_[2]));
  }
  if (name == "moveto") {
    const auto v = numbers(args);
    if (!v || v->size() != 6) return error(3);
    start_move({(*v)[0], (*v)[1], (*v)[2]}, {(*v)[3], (*v)[4], (*v)[5]});
    return kSilent;
  }
  if (name == "stop") {
    speed_ = {};
    return kSilent;
  }
  return error(2);
}

std::string ChromiumSim::scans(std::string_view name, std::string_view args) {
  if (name == "count?") return line(std::to_string(scans_.size()));
  if (name == "status_verbosity") return number(args) ? kSilent : error(3);
  if (name == "stop") return kSilent;
  if (name == "moveto" || name == "inpos?") {
    const auto v = number(args);
    if (!v || *v < 1 || *v > static_cast<double>(scans_.size()) || *v != std::floor(*v)) return error(3);
    const Microns& scan = scans_[static_cast<std::size_t>(*v) - 1];
    const std::array<double, 3> start{static_cast<double>(scan.x), static_cast<double>(scan.y),
                                      static_cast<double>(scan.z)};
    if (name == "moveto") {
      start_move(start, {kScanMoveSpeed, kScanMoveSpeed, kScanMoveSpeed});
      return kSilent;
    }
    return line(at_rest() && at_ == start ? "1" : "0");
  }
  return error(2);
}

}  // namespace pychron::extraction
