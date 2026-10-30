#pragma once

// What a driver sends to open a session on a scripted Qtegra wire.

#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/transport/sim_transport.hpp"
#include "spectrometer/legacy/sim_util.hpp"

namespace qtegra_test {

// Describe's reply for an instrument with the driver's default channels.
inline constexpr std::string_view kDescribe =
    "{\"protocol\":1,\"version\":\"20.2\",\"instrument\":\"Argus\",\"transport\":\"tcp\",\"tag_data\":true,"
    "\"features\":[\"keepalive\",\"describe\"],\"detectors\":[{\"id\":\"CUP 5\",\"name\":\"H2\"},"
    "{\"id\":\"CUP 4\",\"name\":\"H1\"},{\"id\":\"CUP 3\",\"name\":\"AX\"},{\"id\":\"CUP 2\",\"name\":\"L1\"},"
    "{\"id\":\"CUP 1\",\"name\":\"L2\"},{\"id\":\"CDD 0\",\"name\":\"CDD\"}]}\n";

// KeepAlive and Describe answered, then the owner's GetIntegrationTime.
inline std::vector<pychron::SimStep> session_steps(std::string_view terminator = "\r",
                                                   std::string_view integration = "1.048576\n") {
  const auto tx = [&](std::string_view verb) { return std::string(verb) + std::string(terminator); };
  return {legacy_test::step(tx("KeepAlive"), "OK\n"), legacy_test::step(tx("Describe"), kDescribe),
          legacy_test::step(tx("GetIntegrationTime"), integration)};
}

// A scripted wire that starts with a session.
inline std::unique_ptr<pychron::SimTransport> open_session(std::vector<pychron::SimStep> steps,
                                                           std::string_view terminator = "\r") {
  auto all = session_steps(terminator);
  all.insert(all.end(), std::make_move_iterator(steps.begin()), std::make_move_iterator(steps.end()));
  return legacy_test::open_scripted(std::move(all));
}

}  // namespace qtegra_test
