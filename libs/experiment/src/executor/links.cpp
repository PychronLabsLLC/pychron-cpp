#include "pychron/experiment/executor/links.hpp"

#include <array>
#include <cstdio>

#include "pychron/transport/transport.hpp"

namespace pychron::experiment::executor {

LinkSet required_links(const RunSpec& run, const std::vector<Link*>& line, const std::vector<Link*>& spectrometer) {
  LinkSet out;
  const auto take = [&](const std::vector<Link*>& links) {
    for (Link* l : links)
      if (l != nullptr && !l->policy().optional) out.required.push_back(l);
  };
  take(line);
  if (!run.measurement.plan.empty()) take(spectrometer);
  return out;
}

std::optional<CheckFailure> LinksUp::check(const RunSpec& run) {
  std::vector<std::string> down;
  std::string what;
  for (Link* l : links_(run).required) {
    const Health h = l->transport().health();
    if (h.state != HealthState::Down) continue;
    down.push_back(l->transport().name());
    what += (what.empty() ? "" : ", ") + l->transport().name() + (h.last_error.empty() ? "" : " (" + h.last_error + ")");
  }
  if (down.empty()) return std::nullopt;
  return CheckFailure{CheckKind::Wait, Error{ErrorKind::Io, (down.size() == 1 ? "link down: " : "links down: ") + what, {}},
                      std::move(down)};
}

std::optional<CheckFailure> SpoolWritable::check(const RunSpec&) {
  std::array<char, 160> text{};
  if (const auto free = free_mb_ ? free_mb_() : std::nullopt; free && *free < limits_.min_free_mb) {
    std::snprintf(text.data(), text.size(), "spool disk: %.0f MB free, under %.0f MB", *free, limits_.min_free_mb);
    return CheckFailure{CheckKind::Wait, Error{ErrorKind::Io, text.data(), {}}, {}};
  }
  if (const std::size_t pending = pending_ ? pending_() : 0; pending > limits_.max_pending) {
    std::snprintf(text.data(), text.size(), "spool: %zu records waiting to be sent, over %zu", pending, limits_.max_pending);
    return CheckFailure{CheckKind::Wait, Error{ErrorKind::Io, text.data(), {}}, {}};
  }
  return std::nullopt;
}

}  // namespace pychron::experiment::executor
