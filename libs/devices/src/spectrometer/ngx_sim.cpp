#include "pychron/devices/spectrometer/ngx_sim.hpp"

#include <cstdio>
#include <ranges>
#include <sstream>

#include "pychron/codecs/isotopx_ngx.hpp"

namespace pychron::spectrometer {

namespace ngx = codec::ngx;

namespace {

const Clock& steady() {
  static const SteadyClock clock;
  return clock;
}

std::string strip(std::string s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == '#')) s.pop_back();
  return s;
}

std::string clock_text(TimePoint t) {
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count();
  const long long day = 86400LL * 1000000LL;
  long long in_day = us % day;
  if (in_day < 0) in_day += day;
  const long long s = in_day / 1000000, frac = in_day % 1000000;
  char buf[32];
  std::snprintf(buf, sizeof buf, "%02lld:%02lld:%02lld.%06lld", s / 3600, (s / 60) % 60, s % 60, frac);
  return buf;
}

std::string event_line(const NgxSimModel& m, bool baseline, TimePoint at) {
  std::string line = std::string("#EVENT:") + (baseline ? "ACQ.B," : "ACQ,") + m.run->rcs_id + ",SIM,SIM," + clock_text(at);
  for (double value : std::views::reverse(m.values)) line += "," + ngx::format_float(value);
  return line + "#\r\n";
}

// Splits "Verb args" into verb and args (args trimmed).
std::pair<std::string, std::string> verb_args(const std::string& cmd) {
  const auto sp = cmd.find(' ');
  if (sp == std::string::npos) return {cmd, {}};
  std::string args = cmd.substr(sp + 1);
  while (!args.empty() && args.front() == ' ') args.erase(0, 1);
  return {cmd.substr(0, sp), args};
}

std::string reply_for(NgxSimModel& m, const std::string& cmd) {
  const auto [verb, args] = verb_args(cmd);
  const Clock& clock = m.clock != nullptr ? *m.clock : steady();
  if (verb == "Login") {
    const auto comma = args.find(',');
    m.user = args.substr(0, comma);
    m.password = comma == std::string::npos ? std::string{} : args.substr(comma + 1);
    m.logged_in = m.login_reply == "E00";
    return m.login_reply;
  }
  if (verb == "StartAcq") {
    if (m.start_acq_reply != "E00") return m.start_acq_reply;
    if (m.run && !m.run->done) return "E43";  // already acquiring (pychron's duplicate StartAcq)
    NgxSimModel::Run run;
    // NOLINTNEXTLINE(bugprone-unchecked-string-to-number-conversion): the simulator: what is not a number is 0
    run.seconds = std::max(1, std::atoi(args.c_str()));
    run.rcs_id = args.substr(args.find(',') + 1);
    run.start = clock.now();
    m.run = run;
    ++m.runs_started;
    return "E00";
  }
  if (verb == "StopAcq") {
    m.run.reset();
    return "E00";
  }
  if (verb == "SetAcqPeriod") return "E00";
  if (verb == "SAB") {
    m.sab = args == "1";
    return "E00";
  }
  if (verb == "GETMASS") return ngx::format_float(m.mass);
  if (verb == "SetMass") {
    // NOLINTNEXTLINE(bugprone-unchecked-string-to-number-conversion): the simulator: what is not a number is 0
    m.mass = std::atof(args.c_str());
    return "E00";
  }
  if (verb == "SSO") {
    const auto comma = args.find(',');
    if (comma == std::string::npos) return "E05";
    // NOLINTNEXTLINE(bugprone-unchecked-string-to-number-conversion): the simulator: what is not a number is 0
    m.params[args.substr(0, comma)] = std::atof(args.substr(comma + 1).c_str());
    return "E00";
  }
  if (verb == "GSO") {
    const double v = m.params.contains(args) ? m.params[args] : 0.0;
    return ngx::format_float(v) + "," + ngx::format_float(v);
  }
  if (verb == "OpenValve" || verb == "CloseValve") {
    if (!m.sab) ++m.unbracketed_actuations;
    m.valves[args] = verb == "OpenValve";
    return "E00";
  }
  if (verb == "GetValveStatus") {
    if (m.status_e00 > 0) {
      --m.status_e00;
      return "E00";
    }
    return m.valves[args] ? "OPEN" : "CLOSED";
  }
  return "E01";
}

}  // namespace

std::string NgxSimModel::due_locked() {
  std::string out;
  if (banner_pending) {
    banner_pending = false;
    out += banner + reply_terminator;
  }
  if (release_due) {
    release_due = false;
    for (auto& r : held) out += r;
    held.clear();
  }
  if (run && !run->done) {
    const Clock& c = clock != nullptr ? *clock : steady();
    const TimePoint now = c.now();
    while (run->emitted < run->seconds && now >= run->start + std::chrono::seconds(run->emitted + 1)) {
      ++run->emitted;
      out += event_line(*this, false, run->start + std::chrono::seconds(run->emitted));
    }
    if (run->emitted == run->seconds) {
      if (emit_acq_b) out += event_line(*this, true, run->start + std::chrono::seconds(run->seconds));
      run->done = true;
    }
  }
  return out;
}

SimTransport::Hook ngx_sim_hook(std::shared_ptr<NgxSimModel> model) {
  return [model](const Bytes& tx) -> Bytes {
    std::lock_guard lock(model->mutex);
    const std::string cmd = strip(::pychron::to_string(tx));
    model->commands.push_back(cmd);
    std::string reply = reply_for(*model, cmd) + model->reply_terminator;
    if (!model->event_before_next_reply.empty()) {
      reply = std::exchange(model->event_before_next_reply, std::string{}) + reply;
    }
    if (model->hold_replies > 0) {
      --model->hold_replies;
      model->held.push_back(reply);
      return {};
    }
    return to_bytes(reply);
  };
}

SimTransport::Unsolicited ngx_sim_events(std::shared_ptr<NgxSimModel> model) {
  return [model]() -> Bytes {
    std::lock_guard lock(model->mutex);
    return to_bytes(model->due_locked());
  };
}

}  // namespace pychron::spectrometer
