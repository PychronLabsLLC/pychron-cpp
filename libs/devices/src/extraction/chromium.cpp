#include "pychron/devices/extraction/chromium.hpp"

#include <algorithm>
#include <array>
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
// Error::code when the query after an action was refused, not the action.
constexpr std::string_view kConfirmRefused = "chromium-confirm?";

// True when Chromium refused the action itself: nothing was changed.
bool action_refused(const Error& e) { return e.code.starts_with("chromium?"); }
// True for any "?<n>" from Chromium: it answered, and said no.
bool refused(const Error& e) { return e.code.starts_with("chromium"); }
constexpr int kArrivedAfter = 3;          // good polls in a row
constexpr std::array<std::string_view, 3> kAxes{"x", "y", "z"};

// "s3" / "S3": scan 3 of Chromium's scan list.
std::optional<int> scan_number(std::string_view position) {
  if (position.size() < 2 || (position[0] != 's' && position[0] != 'S')) return std::nullopt;
  int n = 0;
  for (const char c : position.substr(1)) {
    if (c < '0' || c > '9' || n > 100000) return std::nullopt;
    n = n * 10 + (c - '0');
  }
  return n;
}

// The command as sent, without its terminator, for error messages.
std::string text_of(const codec::Command& command) {
  std::string text = pychron::to_string(command.tx);
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
  return text;
}

// A percentage for a message: as the codec would send it (no locale).
std::string percent(double value) {
  if (auto command = cr::laser_output(std::clamp(value, 0.0, 100.0)); command && value >= 0 && value <= 100) {
    std::string text = pychron::to_string(command->tx);
    text.pop_back();  // the LF
    return text.substr(text.find(' ') + 1);
  }
  return std::to_string(value);
}

std::string joined(const std::vector<std::string>& names) {
  std::string out;
  for (const auto& n : names) out += (out.empty() ? "" : ", ") + n;
  return out;
}

}  // namespace

namespace {

// The numbers under `key`, if present: exactly `count` of them.
Result<std::optional<std::vector<double>>> numbers(const toml::table& options, std::string_view key, std::size_t count) {
  const toml::array* array = options[key].as_array();
  if (array == nullptr) return std::optional<std::vector<double>>{};
  std::vector<double> out;
  for (const auto& node : *array) out.push_back(node.value<double>().value_or(0));
  if (out.size() != count) {
    return fail(ErrorKind::Config, std::string(key) + " must have " + std::to_string(count) + " values");
  }
  return std::optional<std::vector<double>>(std::move(out));
}

}  // namespace

DriverSchema ChromiumLaser::schema() {
  return {"",
          "Photon Machines Chromium laser system over TCP: output (percent), firing, interlocks, XYZ stage, scan "
          "positions",
          {{"x_limits", KeyType::FloatArray, false, "stage travel in x, mm: [low, high]; default [0, 50]"},
           {"y_limits", KeyType::FloatArray, false, "stage travel in y, mm; default [0, 50]"},
           {"z_limits", KeyType::FloatArray, false, "stage travel in z, mm; default [0, 50]"},
           {"signs", KeyType::IntegerArray, false, "1 or -1 per axis: stage mm times this is sent; default [1, 1, 1]"},
           {"move_speed", KeyType::IntegerArray, false,
            "x, y, z speeds in microns per second; z may be 0 (never moved); default [5000, 5000, 100]"},
           {"in_position_um", KeyType::Float, false, "how near the target counts as arrived, microns; default 10"},
           {"use_enable", KeyType::Boolean, false,
            "send Laser.Enable; false for a unit that refuses it; default true"}},
          true};
}

Result<std::unique_ptr<ChromiumLaser>> ChromiumLaser::create(const DriverArgs& args) {
  ChromiumOptions o;
  const std::array<std::string_view, 3> limit_keys{"x_limits", "y_limits", "z_limits"};
  for (std::size_t i = 0; i < 3; ++i) {
    auto limits = numbers(args.options, limit_keys[i], 2);
    if (!limits) return fail(std::move(limits).error());
    if (!*limits) continue;
    if (!((**limits)[0] < (**limits)[1])) {
      return fail(ErrorKind::Config, std::string(limit_keys[i]) + " must be [low, high] with low below high");
    }
    o.limits_mm[i] = {(**limits)[0], (**limits)[1]};
  }
  if (auto signs = numbers(args.options, "signs", 3); !signs) {
    return fail(std::move(signs).error());
  } else if (*signs) {
    for (std::size_t i = 0; i < 3; ++i) {
      if ((**signs)[i] != 1 && (**signs)[i] != -1) return fail(ErrorKind::Config, "signs must each be 1 or -1");
      o.signs[i] = static_cast<int>((**signs)[i]);
    }
  }
  if (auto speed = numbers(args.options, "move_speed", 3); !speed) {
    return fail(std::move(speed).error());
  } else if (*speed) {
    if ((**speed)[0] <= 0 || (**speed)[1] <= 0 || (**speed)[2] < 0) {
      return fail(ErrorKind::Config, "move_speed must be positive in x and y, and not negative in z");
    }
    o.move_speed = {static_cast<std::int64_t>((**speed)[0]), static_cast<std::int64_t>((**speed)[1]),
                    static_cast<std::int64_t>((**speed)[2])};
  }
  if (const auto near = args.options["in_position_um"].value<double>()) {
    if (!(*near > 0)) return fail(ErrorKind::Config, "in_position_um must be above 0");
    o.in_position_um = *near;
  }
  o.use_enable = args.options["use_enable"].value_or(true);
  DeviceOptions device;
  device.clock = args.clock;
  return std::make_unique<ChromiumLaser>(args.name, args.transport, o, device);
}

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
      // Whose refusal? If the action's, the confirm's own reply follows it:
      // take that off the wire. If nothing follows, the action stood and it
      // was the confirm that Chromium refused.
      auto second = transport_.poll(*confirm.reply);
      if (second && second->has_value()) return fail(cr::to_error(*code, text_of(action)));
      Error e = cr::to_error(*code, text_of(confirm));
      e.code = std::string(kConfirmRefused) + std::to_string(*code);
      return fail(std::move(e));
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
    // A nicety, and a unit with no scan list refuses it: only a dead wire fails here.
    auto verbose = act(cr::scans_status_verbosity(1), cr::sys_id());
    if (!verbose && !refused(verbose.error())) return fail(std::move(verbose).error());
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

Result<void> ChromiumLaser::zero_output() {
  auto zero = cr::laser_output(0);
  if (!zero) return fail(std::move(zero).error());
  auto reply = act(*zero, cr::laser_output_query());
  if (!reply) return fail(std::move(reply).error());
  auto set = cr::decode_number(*reply);
  if (!set) return fail(std::move(set).error());
  if (std::abs(*set) > kOutputTolerance) {
    return fail(ErrorKind::Io, "asked for 0 % output, Chromium still reports " + percent(*set) + " %");
  }
  return {};
}

Result<void> ChromiumLaser::disable() {
  // Everything is tried; the first failure is what is reported.
  Result<void> first;
  auto keep = [&](Result<void> r) {
    if (!r && first) first = std::move(r);
  };
  auto done = [](Result<Bytes> r) -> Result<void> {
    if (!r) return fail(std::move(r).error());
    return {};
  };
  keep(done(act(cr::laser_stop(), cr::sys_id())));
  // A unit with no scan list refuses this; that is not a failure to disable.
  if (auto stopped = act(cr::scans_stop(), cr::sys_id()); !stopped && !refused(stopped.error())) {
    keep(fail(std::move(stopped).error()));
  }
  keep(zero_output());
  if (options_.use_enable) {
    auto off = [&]() -> Result<void> {
      auto reply = act(cr::laser_enable(false), cr::laser_enabled());
      if (!reply) return fail(std::move(reply).error());
      auto on = cr::decode_flag(*reply);
      if (!on) return fail(std::move(on).error());
      if (*on) return fail(ErrorKind::Io, "Chromium did not disable the laser");
      return {};
    };
    keep(off());
  }
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
      return fail(ErrorKind::Protocol,
                  "asked for " + percent(value) + " % output, Chromium reports " + percent(*set) + " %");
    }
    std::lock_guard lock(state_);
    output_ = value;
    return {};
  };
  auto result = run();
  if (!result && !action_refused(result.error())) {
    // The command went out and what the output is now is not known: it must
    // not be left at a setpoint the caller was told had failed.
    (void)zero_output();
    std::lock_guard lock(state_);
    output_ = 0;
  }
  return observe(std::move(result));
}

Result<void> ChromiumLaser::end_extract() {
  Result<void> first;
  if (auto stopped = act(cr::laser_stop(), cr::sys_id()); !stopped) first = fail(std::move(stopped).error());
  if (auto zeroed = zero_output(); !zeroed && first) first = std::move(zeroed);
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
  if (auto ok = observe(check_interlocks("fire")); !ok) return ok;
  auto run = [&]() -> Result<void> {
    auto reply = act(cr::laser_fire(), cr::laser_status());
    if (!reply) return fail(std::move(reply).error());
    // An interlock can trip between the check and the command, and a Chromium
    // that then simply does not fire says so only here.
    auto status = cr::decode_number(*reply);
    if (!status) return fail(std::move(status).error());
    if (*status != 0) return check_interlocks("fire");
    return {};
  };
  auto result = run();
  if (!result) {
    // Laser.Fire may have gone out. Whatever went wrong after it, the beam
    // is not left on behind an error (a refused Laser.Fire never opened it).
    if (!action_refused(result.error())) (void)act(cr::laser_stop(), cr::sys_id());
    std::lock_guard lock(state_);
    firing_ = false;
    return observe(std::move(result));
  }
  {
    std::lock_guard lock(state_);
    firing_ = true;
  }
  return observe(std::move(result));
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

// ---- stage -----------------------------------------------------------------------

cr::Microns ChromiumLaser::to_wire(const StagePosition& mm) const {
  auto um = [&](double v, std::size_t axis) { return std::llround(v * 1000.0) * options_.signs[axis]; };
  return {um(mm.x, 0), um(mm.y, 1), um(mm.z, 2)};
}

StagePosition ChromiumLaser::from_wire(const cr::Microns& um) const {
  auto mm = [&](std::int64_t v, std::size_t axis) { return static_cast<double>(v * options_.signs[axis]) / 1000.0; };
  return {mm(um.x, 0), mm(um.y, 1), mm(um.z, 2)};
}

Result<StagePosition> ChromiumLaser::read_position() {
  auto reply = query(cr::stage_position());
  if (!reply) return fail(std::move(reply).error());
  auto at = cr::decode_position(*reply);
  if (!at) return fail(std::move(at).error());
  return from_wire(*at);
}

Result<StagePosition> ChromiumLaser::position() { return observe(read_position()); }

Result<void> ChromiumLaser::check_travel(std::size_t axis, double mm) const {
  const auto [low, high] = options_.limits_mm[axis];
  if (std::isfinite(mm) && mm >= low && mm <= high) return {};
  return refuse(ErrorKind::Config, std::string(kAxes[axis]) + " = " + std::to_string(mm) +
                                       " mm is outside the stage's travel (" + std::to_string(low) + " to " +
                                       std::to_string(high) + " mm)");
}

Result<void> ChromiumLaser::start_move(const StagePosition& to, double speed_mm_s, const StagePosition* from) {
  const std::array<double, 3> wanted{to.x, to.y, to.z};
  for (std::size_t i = 0; i < 3; ++i) {
    if (auto ok = check_travel(i, wanted[i]); !ok) return ok;
  }
  const cr::Microns wire = to_wire(to);
  cr::Microns speed = options_.move_speed;
  if (speed_mm_s > 0) {
    // Whole microns per second, at most the stage's own and at least 1: a
    // speed of 0 on the wire is a move that never arrives.
    const double um_per_s = std::min(speed_mm_s * 1000.0, 1e12);
    // Each axis runs at its own speed: for a straight line at `speed_mm_s`
    // each gets its share of the distance. An axis that does not move keeps
    // the whole speed (it has nowhere to go).
    double share_x = 1, share_y = 1;
    if (from != nullptr) {
      // In whole microns, as the stage sees it: an axis whose target is
      // where it already is does not move, whatever a double says.
      const cr::Microns start = to_wire(*from);
      const double dx = std::abs(static_cast<double>(wire.x - start.x));
      const double dy = std::abs(static_cast<double>(wire.y - start.y));
      const double d = std::hypot(dx, dy);
      if (dx > 0) share_x = dx / d;
      if (dy > 0) share_y = dy / d;
    }
    const auto whole = [](double v) { return static_cast<std::int64_t>(std::llround(v)); };
    speed.x = std::clamp<std::int64_t>(whole(um_per_s * share_x), 1, std::max<std::int64_t>(1, options_.move_speed.x));
    speed.y = std::clamp<std::int64_t>(whole(um_per_s * share_y), 1, std::max<std::int64_t>(1, options_.move_speed.y));
  }
  auto reply = act(cr::stage_move_to(wire, speed), cr::stage_position());
  if (!reply) return observe(Result<void>(fail(std::move(reply).error())));
  std::lock_guard lock(state_);
  target_ = Target{std::nullopt, wire};
  good_polls_ = 0;
  return observe(Result<void>{});
}

Result<void> ChromiumLaser::set_xy(double x, double y, double speed_mm_s) {
  if (!std::isfinite(speed_mm_s) || speed_mm_s < 0) {
    return refuse(ErrorKind::Config, "a stage speed must be 0 (the stage's own) or more, in mm/s");
  }
  // Checked before the read, so a move that cannot be made sends nothing.
  if (auto ok = check_travel(0, x); !ok) return ok;
  if (auto ok = check_travel(1, y); !ok) return ok;
  // z stays where it is: read, not remembered.
  auto at = observe(read_position());
  if (!at) return fail(std::move(at).error());
  return start_move({x, y, at->z}, speed_mm_s, &*at);
}

Result<void> ChromiumLaser::stop() {
  auto reply = act(cr::stage_stop(), cr::stage_position());
  // Whatever Chromium said, the driver no longer knows where the stage is
  // going: moving() must not go on reporting a move it cannot vouch for.
  {
    std::lock_guard lock(state_);
    target_.reset();
    good_polls_ = 0;
  }
  if (!reply) return observe(Result<void>(fail(std::move(reply).error())));
  return observe(Result<void>{});
}

Result<void> ChromiumLaser::set_axis(Axis axis, double value) {
  if (auto ok = check_travel(axis == Axis::X ? 0 : axis == Axis::Y ? 1 : 2, value); !ok) return ok;
  auto at = observe(read_position());
  if (!at) return fail(std::move(at).error());
  StagePosition to = *at;
  (axis == Axis::X ? to.x : axis == Axis::Y ? to.y : to.z) = value;
  return start_move(to);
}

Result<void> ChromiumLaser::move_to_position(std::string_view position, bool /*autocenter*/) {
  if (const auto scan = scan_number(position)) {
    auto move = cr::scan_move_to(*scan);
    auto in_position = cr::scan_in_position(*scan);
    if (!move || !in_position) return refuse(ErrorKind::Config, "no scan " + std::string(position));
    auto reply = act(*move, *in_position);
    if (!reply) return observe(Result<void>(fail(std::move(reply).error())));
    std::lock_guard lock(state_);
    target_ = Target{*scan, {}};
    good_polls_ = 0;
    return observe(Result<void>{});
  }
  // A hole on a tray is the laser system's to resolve (pychron/laser).
  return refuse(ErrorKind::Config, "'" + std::string(position) + "' is not a scan position (s<n>)");
}

Result<bool> ChromiumLaser::moving() {
  std::optional<Target> target;
  {
    std::lock_guard lock(state_);
    target = target_;
  }
  if (!target) return false;

  auto clear = [&] {
    std::lock_guard lock(state_);
    target_.reset();
    good_polls_ = 0;
  };
  auto run = [&]() -> Result<bool> {
    // A stage on a limit switch is not going to arrive.
    auto status = query(cr::stage_limits());
    if (!status) return fail(std::move(status).error());
    auto limits = cr::decode_limits(*status);
    if (!limits) return fail(std::move(limits).error());
    const std::array<int, 3> on{limits->x, limits->y, limits->z};
    for (std::size_t i = 0; i < 3; ++i) {
      if (on[i] != 0) {
        clear();
        return fail(ErrorKind::Io, "the stage hit its " + std::string(on[i] > 0 ? "positive" : "negative") + " " +
                                       std::string(kAxes[i]) + " limit switch");
      }
    }
    bool arrived = false;
    if (target->scan) {
      auto ask = cr::scan_in_position(*target->scan);
      if (!ask) return fail(std::move(ask).error());
      auto reply = query(*ask);
      if (!reply) return fail(std::move(reply).error());
      auto there = cr::decode_flag(*reply);
      if (!there) return fail(std::move(there).error());
      arrived = *there;
    } else {
      auto reply = query(cr::stage_position());
      if (!reply) return fail(std::move(reply).error());
      auto at = cr::decode_position(*reply);
      if (!at) return fail(std::move(at).error());
      auto near = [&](std::int64_t a, std::int64_t b) {
        return std::abs(static_cast<double>(a - b)) <= options_.in_position_um;
      };
      arrived = near(at->x, target->at.x) && near(at->y, target->at.y) && near(at->z, target->at.z);
    }
    std::lock_guard lock(state_);
    good_polls_ = arrived ? good_polls_ + 1 : 0;
    if (good_polls_ < kArrivedAfter) return true;
    target_.reset();
    good_polls_ = 0;
    return false;
  };
  return observe(run());
}

// Trays are the laser system's; the driver keeps the name and nothing else.
Result<void> ChromiumLaser::set_tray(std::string_view tray) {
  std::lock_guard lock(state_);
  tray_ = std::string(tray);
  return {};
}

std::vector<std::string> ChromiumLaser::positions() const { return {}; }

}  // namespace pychron::extraction

REGISTER_DRIVER("chromium", pychron::extraction::ChromiumLaser);
