#include "pychron/scripting/duration_accumulator.hpp"

#include <algorithm>

namespace pychron::scripting {

void DurationAccumulator::add(Duration d, std::string command, std::string script, int line) {
  if (d < Duration::zero()) d = Duration::zero();
  total_ += d;
  entries_.push_back({std::move(script), line, std::move(command), d});
}

void DurationAccumulator::flag_unbounded(std::string reason) {
  if (std::find(unbounded_.begin(), unbounded_.end(), reason) == unbounded_.end())
    unbounded_.push_back(std::move(reason));
}

}  // namespace pychron::scripting
