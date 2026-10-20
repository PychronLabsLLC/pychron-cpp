#pragma once

// Rehearsal: a script run in which every hardware call and wait goes through
// one IRehearsal::step and nothing touches hardware (IScriptHost::rehearse).
// The caller decides when each step may go on and what it answers.

#include <string>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/scripting/script.hpp"

namespace pychron::scripting {

struct StepCall {
  std::string command;              // vocabulary name: "open", "sleep", "extract", ...
  std::vector<Value> args;          // positional, as the script passed them
  ValueMap kwargs;                  // keyword arguments
  std::string script;               // script (or gosub) the call is in
  int line = 0;                     // its line there
  std::vector<std::string> gosubs;  // outermost first; empty in the top script
};

struct IRehearsal {
  virtual ~IRehearsal() = default;
  // Called on the script's thread with the GIL released. Blocks until the
  // step may go on. Returns the call's value (monostate for a void call),
  // an error the script sees as the real call's error, or Cancelled.
  virtual Result<Value> step(const StepCall& call) = 0;
};

}  // namespace pychron::scripting
