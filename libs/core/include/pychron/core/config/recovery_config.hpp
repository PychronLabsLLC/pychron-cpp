#pragma once

// `[transports.<name>.recovery]`: how the link over a transport recovers
// (executor hardening spec, sections 4.2, 4.3 and 4.10). Read by the line's
// loader and by the spectrometer's; turned into a `LinkPolicy`
// (`libs/devices` `link.hpp`, `link_policy()`) where the transport is built.
// The defaults are LinkPolicy's.

#include <optional>

#include "pychron/core/config/located.hpp"

namespace pychron::config {

struct RecoveryConfig : Located {
  bool reconnect = true;       // false: a failed call is not retried, the link not reopened
  bool optional = false;       // a run may start without this link (spec 4.3)
  // How long one call keeps trying, 0 to 3600; absent: the lab's default
  // (executor.toml [links], itself 20 s by default).
  std::optional<double> call_window_s;

  static constexpr double kMaxCallWindowS = 3600.0;
};

}  // namespace pychron::config
