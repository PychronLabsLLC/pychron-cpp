#pragma once

// Which links a run needs, and the pre-run checks that ask the queue to wait
// (executor hardening spec, sections 4.3 and 4.6).

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "pychron/devices/link.hpp"
#include "pychron/experiment/executor/executor.hpp"

namespace pychron::experiment::executor {

// The links a run needs, derived, never configured (spec 4.3): every link
// of the line (its scripts may reach any valve, switch or gauge, and its
// extraction device is a driver on it), and the spectrometer's when the run
// has a measurement plan. A link whose transport is optional
// ([transports.<name>.recovery] optional = true) is left out.
struct LinkSet {
  std::vector<Link*> required;
};

[[nodiscard]] LinkSet required_links(const RunSpec& run, const std::vector<Link*>& line,
                                     const std::vector<Link*>& spectrometer);

// Waits while a link the next run needs is down (its transport's health is
// Down: closed, or as many failures in a row as the transport allows).
class LinksUp final : public IPreRunCheck {
 public:
  explicit LinksUp(std::function<LinkSet(const RunSpec&)> links) : links_(std::move(links)) {}
  std::string name() const override { return "links"; }
  std::optional<CheckFailure> check(const RunSpec& run) override;

 private:
  std::function<LinkSet(const RunSpec&)> links_;
};

// Waits while the spool cannot be relied on: under min_free_mb free on its
// disk, or over max_pending records waiting to be sent (spec 4.6).
class SpoolWritable final : public IPreRunCheck {
 public:
  struct Limits {
    double min_free_mb = 500;
    std::size_t max_pending = 200;
  };
  // `free_mb`: the spool disk's free space (nullopt: unknown, not a reason to
  // wait); `pending`: records not yet sent.
  SpoolWritable(Limits limits, std::function<std::optional<double>()> free_mb, std::function<std::size_t()> pending)
      : limits_(limits), free_mb_(std::move(free_mb)), pending_(std::move(pending)) {}
  std::string name() const override { return "spool"; }
  std::optional<CheckFailure> check(const RunSpec& run) override;

 private:
  Limits limits_;
  std::function<std::optional<double>()> free_mb_;
  std::function<std::size_t()> pending_;
};

}  // namespace pychron::experiment::executor
