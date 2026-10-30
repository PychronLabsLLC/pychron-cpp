#include "pychron/sim/wire_sim.hpp"

#include <utility>

namespace pychron::sim {

RequestFramer framer(ReadSpec spec) {
  return [spec = std::move(spec)](const Bytes& buffer) { return frame_length(spec, buffer); };
}

}  // namespace pychron::sim
