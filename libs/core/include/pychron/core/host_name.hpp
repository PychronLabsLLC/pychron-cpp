#pragma once

#include <string>

namespace pychron {

// This computer's name, as the system has it (POSIX gethostname; on Windows
// the COMPUTERNAME variable, which the system sets for every process). Never
// empty: "localhost" when the system gives none. Not the HOSTNAME variable:
// a shell sets that for itself and rarely exports it, so a program that
// asked for it was "localhost" on every machine.
std::string host_name();

}  // namespace pychron
