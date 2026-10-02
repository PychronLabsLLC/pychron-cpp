#pragma once

// Optional driver capability: a handshake that needs an open transport (a
// vendor protocol that must log in or negotiate before the first command).
// Drivers without one simply do not implement it.

#include "pychron/core/error.hpp"

namespace pychron {

struct IConnectable {
  virtual ~IConnectable() = default;
  // Called once after every transport of the system is open; blocks until the
  // device answers or the transport times out.
  virtual Result<void> connect() = 0;
};

}  // namespace pychron
