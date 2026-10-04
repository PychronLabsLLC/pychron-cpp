// elctl entry without the DVC store: the command exists and says why it does nothing.

#include <ostream>

#include "entry.hpp"

namespace elctl {

int entry_command(const std::vector<std::string>&, Io io) {
  io.err << "elctl was built without persistence\n";
  return kUsage;
}

}  // namespace elctl
