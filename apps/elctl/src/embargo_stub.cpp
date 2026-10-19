// elctl embargo without the DVC store (PYCHRON_PERSISTENCE=OFF, or Qt not
// found): the command exists and says why it does nothing.

#include <ostream>

#include "embargo.hpp"

namespace elctl {

int embargo_command(const std::vector<std::string>&, Io io) {
  io.err << "elctl was built without persistence\n";
  return kUsage;
}

}  // namespace elctl
