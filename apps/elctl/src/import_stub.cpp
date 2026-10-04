// elctl import without the DVC store (PYCHRON_PERSISTENCE=OFF, or Qt not
// found): the command exists and says why it does nothing.

#include <ostream>

#include "import.hpp"

namespace elctl {

int import_command(const std::vector<std::string>&, Io io) {
  io.err << "elctl was built without persistence\n";
  return kUsage;
}

void set_import_batch_hook(std::function<void()>) {}

}  // namespace elctl
