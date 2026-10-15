#include "pychron/experiment/lab/executor_config.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <sstream>
#include <vector>

#include <toml++/toml.hpp>

namespace pychron::experiment::lab {

namespace {

pychron::Duration seconds(double s) {
  return std::chrono::duration_cast<pychron::Duration>(std::chrono::duration<double>(s));
}

std::string shown(double v) {
  std::ostringstream o;
  o << v;
  return o.str();
}

// Collects every problem of one file, each naming its key.
class Reader {
 public:
  explicit Reader(std::string source) : source_(std::move(source)) {}

  // The table `name` of `root`, after its keys are checked; nullptr when absent.
  const toml::table* section(const toml::table& root, std::string_view name, std::initializer_list<std::string_view> keys) {
    const auto* node = root.get(name);
    if (node == nullptr) return nullptr;
    const auto* t = node->as_table();
    if (t == nullptr) {
      problem(*node, std::string(name), "expected a table");
      return nullptr;
    }
    for (auto&& [k, v] : *t) {
      bool known = false;
      for (auto key : keys) known = known || key == k.str();
      if (!known) problem(v, std::string(name) + "." + std::string(k.str()), "unknown key");
    }
    return t;
  }

  // `out` = the number at `key` when it is in [low, high] (low excluded when `open_low`).
  void number(const toml::table* t, std::string_view table, std::string_view key, double low, double high,
              double& out, bool open_low = false) {
    if (t == nullptr) return;
    const auto* node = t->get(key);
    if (node == nullptr) return;
    const std::string path = std::string(table) + "." + std::string(key);
    double v = 0;
    if (const auto* f = node->as_floating_point()) v = f->get();
    else if (const auto* i = node->as_integer()) v = static_cast<double>(i->get());
    else return problem(*node, path, "expected a number");
    const std::string range = std::string(open_low ? "(" : "[") + shown(low) + ", " + shown(high) + "]";
    if (!std::isfinite(v) || v > high || v < low || (open_low && !(v > low)))
      return problem(*node, path, "value " + shown(v) + " out of range, must be in " + range);
    out = v;
  }

  void problem(const toml::node& node, const std::string& key, const std::string& what) {
    errors_.push_back(source_ + ":" + std::to_string(node.source().begin.line) + ":" + key + ": " + what);
  }
  void problem(const std::string& what) { errors_.push_back(source_ + ": " + what); }

  [[nodiscard]] Result<void> result() const {
    if (errors_.empty()) return {};
    std::string all;
    for (const auto& e : errors_) all += (all.empty() ? "" : "\n") + e;
    return fail(ErrorKind::Config, all);
  }

 private:
  std::string source_;
  std::vector<std::string> errors_;
};

}  // namespace

std::string_view to_string(AfterLinkFailure a) noexcept {
  return a == AfterLinkFailure::End ? "end" : "hold_then_continue";
}

LinkPolicy ExecutorConfig::link_defaults() const {
  LinkPolicy p;
  p.call_window = seconds(links.call_window_s);
  p.backoff_min = seconds(links.backoff_min_s);
  p.backoff_max = seconds(links.backoff_max_s);
  return p;
}

pychron::Duration ExecutorConfig::critical_window() const { return seconds(links.critical_window_s); }
pychron::Duration ExecutorConfig::max_gap() const { return seconds(measurement.max_gap_s); }

Result<ExecutorConfig> ExecutorConfig::from_toml(std::string_view text, const std::string& source) {
  auto parsed = toml::parse(text, source);
  if (!parsed) {
    const auto& e = parsed.error();
    return fail(ErrorKind::Config, source + ":" + std::to_string(e.source().begin.line) + ": " +
                                       std::string(e.description()));
  }
  const toml::table& root = parsed.table();
  ExecutorConfig c;
  Reader r(source);
  for (auto&& [k, v] : root) {
    const std::string_view key = k.str();
    if (key != "links" && key != "hold" && key != "measurement" && key != "spool")
      r.problem(v, std::string(key), "unknown table");
  }
  constexpr double kHour = 3600, kDay = 86400, kWeek = 7 * kDay;
  if (const auto* t = r.section(root, "links", {"call_window_s", "critical_window_s", "backoff_min_s", "backoff_max_s"})) {
    r.number(t, "links", "call_window_s", 0, kHour, c.links.call_window_s);
    r.number(t, "links", "critical_window_s", 0, kDay, c.links.critical_window_s);
    r.number(t, "links", "backoff_min_s", 0, kHour, c.links.backoff_min_s, true);
    r.number(t, "links", "backoff_max_s", 0, kHour, c.links.backoff_max_s, true);
    if (c.links.backoff_max_s < c.links.backoff_min_s)
      r.problem(*t, "links.backoff_max_s",
                "value " + shown(c.links.backoff_max_s) + " is under links.backoff_min_s " + shown(c.links.backoff_min_s));
  }
  if (const auto* t = r.section(root, "hold", {"settle_s", "probe_interval_s", "limit_s", "after_link_failure"})) {
    r.number(t, "hold", "settle_s", 0, kHour, c.hold.settle_s);
    r.number(t, "hold", "probe_interval_s", 0, kHour, c.hold.probe_interval_s, true);
    r.number(t, "hold", "limit_s", 0, kWeek, c.hold.limit_s);
    if (const auto* node = t->get("after_link_failure")) {
      const auto v = node->value<std::string>();
      if (v && *v == "hold_then_continue") c.hold.after_link_failure = AfterLinkFailure::HoldThenContinue;
      else if (v && *v == "end") c.hold.after_link_failure = AfterLinkFailure::End;
      else
        r.problem(*node, "hold.after_link_failure",
                  "value " + (v ? "'" + *v + "'" : std::string("not text")) + ", must be 'hold_then_continue' or 'end'");
    }
  }
  if (const auto* t = r.section(root, "measurement", {"max_gap_s", "min_counts_fraction"})) {
    r.number(t, "measurement", "max_gap_s", 0, kHour, c.measurement.max_gap_s);
    r.number(t, "measurement", "min_counts_fraction", 0, 1, c.measurement.min_counts_fraction);
  }
  if (const auto* t = r.section(root, "spool", {"min_free_mb", "max_pending"})) {
    r.number(t, "spool", "min_free_mb", 0, 1e7, c.spool.min_free_mb);
    double pending = c.spool.max_pending;
    r.number(t, "spool", "max_pending", 0, 1e6, pending);
    if (const auto* node = t->get("max_pending"); node != nullptr && !node->is_integer() && node->is_number())
      r.problem(*node, "spool.max_pending", "expected a whole number");
    c.spool.max_pending = static_cast<int>(pending);
  }
  if (auto ok = r.result(); !ok) return fail(ok.error());
  return c;
}

Result<ExecutorConfig> ExecutorConfig::load(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return ExecutorConfig{};
  std::ifstream in(path, std::ios::binary);
  if (!in) return fail(ErrorKind::Io, "cannot open " + path.string());
  std::ostringstream ss;
  ss << in.rdbuf();
  return from_toml(ss.str(), path.string());
}

}  // namespace pychron::experiment::lab
