// elctl laser track: the tracking bench (grain simulator design, section 7).

#include <string>

#include "laser.hpp"
#include "pychron/laser/track_bench.hpp"

namespace elctl {

int laser_track(const std::vector<std::string>& words, const pychron::laser::TrackOptions& options, Io io) {
  const auto scenario = pychron::laser::load_track_scenario(words.at(1));
  if (!scenario) {
    io.err << "elctl laser track: " << scenario.error().what << '\n';
    return kFailed;
  }
  const auto combos = pychron::laser::run_track(*scenario, options);
  if (!combos) {
    io.err << "elctl laser track: " << combos.error().what << '\n';
    return kFailed;
  }
  if (options.check_only) {
    io.out << words.at(1) << ": ok\n";
    return kOk;
  }
  io.out << pychron::laser::track_table(*combos);
  return kOk;
}

}  // namespace elctl
