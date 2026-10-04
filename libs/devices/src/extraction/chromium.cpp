#include "pychron/devices/extraction/chromium.hpp"

#include <chrono>
#include <cmath>

#include "pychron/devices/extraction/capability.hpp"

namespace pychron::extraction {

namespace cr = codec::chromium;

namespace {

// How long to look for input nobody is waiting for before an action.
constexpr Duration kDrain = std::chrono::milliseconds(1);
constexpr int kDrainLimit = 16;
constexpr double kOutputTolerance = 0.1;  // percent

// The command as sent, without its terminator, for error messages.
std::string text_of(const codec::Command& command) {
  std::string text = pychron::to_string(command.tx);
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
  return text;
}

std::string joined(const std::vector<std::string>& names) {
  std::string out;
  for (const auto& n : names) out += (out.empty() ? "" : ", ") + n;
  return out;
}

}  // namespace

ChromiumLaser::ChromiumLaser(std::string name, Transport& transport, ChromiumOptions options, DeviceOptions device)
    : Device(std::move(name), device), transport_(transport), options_(options) {}

std::string ChromiumLaser::chromium_id() const {
  std::lock_guard lock(state_);
  return id_;
}

Unexpected<Error> ChromiumLaser::refuse(ErrorKind kind, std::string what) const {
  return fail(kind, std::move(what), name());
}

Result<Bytes> ChromiumLaser::query(const codec::Command& q) {
  auto reply = transport_.exchange(q.tx, *q.reply);
  if (!reply) return fail(std::move(reply).error());
  if (const auto code = cr::error_code(*reply)) return fail(cr::to_error(*code, text_of(q)));
  return reply;
}

Result<Bytes> ChromiumLaser::act(const codec::Command& action, const codec::Command& confirm) {
  return transact(transport_, [&]() -> Result<Bytes> {
    // Anything unread would be taken for this action's verdict.
    for (int i = 0; i < kDrainLimit; ++i) {
      auto stale = transport_.poll(cr::reply_frame(), kDrain);
      if (!stale) return fail(std::move(stale).error());
      if (!*stale) break;
    }
    if (auto sent = transport_.write(action.tx); !sent) return fail(std::move(sent).error());
    if (auto sent = transport_.write(confirm.tx); !sent) return fail(std::move(sent).error());
    auto first = transport_.read(*confirm.reply);
    if (!first) return fail(std::move(first).error());
    if (const auto code = cr::error_code(*first)) {
      // The action was refused. The confirm's own reply follows: take it off the wire.
      (void)transport_.poll(*confirm.reply);
      return fail(cr::to_error(*code, text_of(action)));
    }
    return first;
  });
}

Result<void> ChromiumLaser::check_interlocks(std::string_view doing) {
  auto reply = query(cr::laser_status());
  if (!reply) return fail(std::move(reply).error());
  auto status = cr::decode_number(*reply);
  if (!status) return fail(std::move(status).error());
  if (*status == 0) return {};
  std::string names = "unknown";
  if (auto listed = query(cr::laser_interlocks())) {
    if (auto parsed = cr::decode_interlocks(*listed); parsed && !parsed->empty()) names = joined(*parsed);
  }
  return fail(ErrorKind::Interlock, "cannot " + std::string(doing) + ": laser interlock tripped (" + names + ")");
}

Result<void> ChromiumLaser::prepare() {
  auto run = [&]() -> Result<void> {
    auto reply = query(cr::sys_id());
    if (!reply) return fail(std::move(reply).error());
    auto id = cr::decode_id(*reply);
    if (!id) return fail(std::move(id).error());
    {
      std::lock_guard lock(state_);
      id_ = *id;
    }
    auto verbose = act(cr::scans_status_verbosity(1), cr::sys_id());
    if (!verbose) return fail(std::move(verbose).error());
    return {};
  };
  return observe(run());
}

Result<void> ChromiumLaser::enable() {
  auto run = [&]() -> Result<void> {
    if (auto ok = check_interlocks("enable the laser"); !ok) return ok;
    if (options_.use_enable) {
      auto reply = act(cr::laser_enable(true), cr::laser_enabled());
      if (!reply) return fail(std::move(reply).error());
      auto on = cr::decode_flag(*reply);
      if (!on) return fail(std::move(on).error());
      if (!*on) return fail(ErrorKind::Io, "Chromium did not enable the laser");
    }
    std::lock_guard lock(state_);
    enabled_ = true;
    return {};
  };
  return observe(run());
}

Result<void> ChromiumLaser::disable() {
  // Everything is tried; the first failure is what is reported.
  Result<void> first;
  auto attempt = [&](Result<Bytes> r) {
    if (!r && first) first = fail(std::move(r).error());
  };
  attempt(act(cr::laser_stop(), cr::sys_id()));
  attempt(act(cr::scans_stop(), cr::sys_id()));
  if (auto zero = cr::laser_output(0)) attempt(act(*zero, cr::laser_output_query()));
  if (options_.use_enable) attempt(act(cr::laser_enable(false), cr::laser_enabled()));
  {
    std::lock_guard lock(state_);
    enabled_ = false;
    firing_ = false;
    output_ = 0;
  }
  return observe(std::move(first));
}

Result<bool> ChromiumLaser::is_enabled() {
  if (!options_.use_enable) {
    std::lock_guard lock(state_);
    return enabled_;
  }
  auto run = [&]() -> Result<bool> {
    auto reply = query(cr::laser_enabled());
    if (!reply) return fail(std::move(reply).error());
    return cr::decode_flag(*reply);
  };
  auto on = observe(run());
  if (on) {
    std::lock_guard lock(state_);
    enabled_ = *on;
  }
  return on;
}

Result<void> ChromiumLaser::extract(double value, ExtractUnits units) {
  if (!supports(units)) {
    return fail(not_supported("extract in " + std::string(to_string(units)) + " (a Chromium takes percent)", name()));
  }
  auto command = cr::laser_output(value);
  if (!command) return refuse(ErrorKind::Config, command.error().what);
  {
    std::lock_guard lock(state_);
    if (!enabled_) return refuse(ErrorKind::Interlock, "the laser is not enabled");
  }
  auto run = [&]() -> Result<void> {
    auto reply = act(*command, cr::laser_output_query());
    if (!reply) return fail(std::move(reply).error());
    auto set = cr::decode_number(*reply);
    if (!set) return fail(std::move(set).error());
    if (std::abs(*set - value) > kOutputTolerance) {
      return fail(ErrorKind::Protocol, "asked for " + std::to_string(value) + " % output, Chromium reports " +
                                           std::to_string(*set) + " %");
    }
    std::lock_guard lock(state_);
    output_ = value;
    return {};
  };
  return observe(run());
}

Result<void> ChromiumLaser::end_extract() {
  Result<void> first;
  auto attempt = [&](Result<Bytes> r) {
    if (!r && first) first = fail(std::move(r).error());
  };
  attempt(act(cr::laser_stop(), cr::sys_id()));
  if (auto zero = cr::laser_output(0)) attempt(act(*zero, cr::laser_output_query()));
  {
    std::lock_guard lock(state_);
    firing_ = false;
    output_ = 0;
  }
  return observe(std::move(first));
}

Result<double> ChromiumLaser::output() {
  std::lock_guard lock(state_);
  return output_;
}

Result<void> ChromiumLaser::fire_laser() {
  {
    std::lock_guard lock(state_);
    if (!enabled_) return refuse(ErrorKind::Interlock, "the laser is not enabled");
  }
  auto run = [&]() -> Result<void> {
    if (auto ok = check_interlocks("fire"); !ok) return ok;
    auto reply = act(cr::laser_fire(), cr::laser_status());
    if (!reply) return fail(std::move(reply).error());
    std::lock_guard lock(state_);
    firing_ = true;
    return {};
  };
  return observe(run());
}

Result<void> ChromiumLaser::stop_laser() {
  auto reply = act(cr::laser_stop(), cr::sys_id());
  {
    std::lock_guard lock(state_);
    firing_ = false;
  }
  if (!reply) return observe(Result<void>(fail(std::move(reply).error())));
  return observe(Result<void>{});
}

Result<bool> ChromiumLaser::is_firing() {
  std::lock_guard lock(state_);
  return firing_;
}

}  // namespace pychron::extraction
