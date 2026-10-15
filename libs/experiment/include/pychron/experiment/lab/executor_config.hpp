#pragma once

// <lab>/executor.toml: the limits of link recovery (executor hardening spec,
// section 4.10). Optional: a lab without the file has the defaults below,
// which are the spec's. configs/examples/executor.toml lists every key with
// its default, commented out (ExecutorConfig.TheExampleFileIsTheDefaults).

#include <filesystem>
#include <string>
#include <string_view>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/devices/link.hpp"

namespace pychron::experiment::lab {

enum class AfterLinkFailure { HoldThenContinue, End };

// "hold_then_continue", "end".
[[nodiscard]] std::string_view to_string(AfterLinkFailure a) noexcept;

struct ExecutorConfig {
  struct Links {
    double call_window_s = 20;      // how long one call keeps trying (a transport may say otherwise)
    double critical_window_s = 600;  // a call that must get through: a valve in a measurement, the safe state
    double backoff_min_s = 1;
    double backoff_max_s = 30;
  } links;
  struct Hold {
    double settle_s = 30;           // a link is back once it has been up this long
    double probe_interval_s = 15;   // each link is asked this often while holding
    double limit_s = 7200;          // a hold this long ends the queue
    AfterLinkFailure after_link_failure = AfterLinkFailure::HoldThenContinue;
  } hold;
  struct Measurement {
    double max_gap_s = 120;            // a silence in the readings waited out this long
    double min_counts_fraction = 0.2;  // of main's planned counts, after a gap
  } measurement;
  struct Spool {
    double min_free_mb = 500;  // the spool's disk keeps this free, or the queue waits
    int max_pending = 200;     // records not yet sent, or the queue waits
  } spool;

  // A link's policy where its transport does not say otherwise.
  [[nodiscard]] LinkPolicy link_defaults() const;
  [[nodiscard]] pychron::Duration critical_window() const;
  [[nodiscard]] pychron::Duration max_gap() const;

  // Config error naming the key, its value and the range it must be in, or
  // the unknown key or table; every problem of the file at once.
  static Result<ExecutorConfig> from_toml(std::string_view text, const std::string& source);
  // A missing file is the defaults.
  static Result<ExecutorConfig> load(const std::filesystem::path& path);
};

}  // namespace pychron::experiment::lab
