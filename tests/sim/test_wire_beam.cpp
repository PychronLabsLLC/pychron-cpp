// feed_qtegra_from_beam, feed_ngx_from_beam: the wire emulators measure a
// simulated beam (sim serve spec, phase B). ManualClock, hook level.
#include "pychron/sim/spectrometer/wire_beam.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>

#include "pychron/codecs/isotopx_ngx.hpp"
#include "pychron/codecs/thermo_qtegra.hpp"

namespace {

using namespace pychron;
using namespace std::chrono_literals;
namespace q = codec::qtegra;

double tagged(const Bytes& reply, const std::string& name) {
  auto pairs = q::decode_data(reply);
  EXPECT_TRUE(pairs) << to_string(reply);
  if (!pairs) return 0.0;
  for (const auto& [n, v] : *pairs)
    if (n == name) return v;
  ADD_FAILURE() << name << " not in " << to_string(reply);
  return 0.0;
}

std::shared_ptr<sim::BeamModel> argon_beam(const Clock& clock, std::initializer_list<const char*> detectors) {
  auto beam = std::make_shared<sim::BeamModel>(clock);  // argon, Ar40 at 8.0 on every detector
  for (const char* d : detectors) beam->ensure_detector(d);
  return beam;
}

Bytes ask(const SimTransport::Hook& hook, const std::string& command) { return hook(to_bytes(command + "\r")); }

}  // namespace

TEST(WireBeam, QtegraReadsThePeak) {
  ManualClock clock;
  auto beam = argon_beam(clock, {"H1", "AX"});
  auto model = std::make_shared<spectrometer::QtegraSimModel>();
  sim::feed_qtegra_from_beam(*model, beam);
  auto hook = spectrometer::qtegra_sim_hook(model);
  ask(hook, "SetMagnetDAC 0.1");
  clock.advance(2s);
  const double off = tagged(ask(hook, "GetData"), "H1");
  const auto ar40 = beam->peak_center("H1", "Ar40");
  ASSERT_TRUE(ar40);
  ask(hook, "SetMagnetDAC " + std::to_string(*ar40));
  clock.advance(2s);
  const double on = tagged(ask(hook, "GetData"), "H1");
  EXPECT_GT(on, 100 * std::max(off, 1.0)) << "on " << on << ", off " << off;
}

TEST(WireBeam, QtegraSettingsMoveTheBeam) {
  ManualClock clock;
  auto beam = argon_beam(clock, {"H1"});
  auto model = std::make_shared<spectrometer::QtegraSimModel>();
  sim::feed_qtegra_from_beam(*model, beam);
  EXPECT_DOUBLE_EQ(model->hv, beam->nominal_hv());
  auto hook = spectrometer::qtegra_sim_hook(model);
  ask(hook, "SetHV 4000");
  EXPECT_DOUBLE_EQ(beam->hv(), 4000.0);
  ask(hook, "SetDeflection H1,10");
  EXPECT_DOUBLE_EQ(beam->detector("H1")->deflection, 10.0);
  ask(hook, "ProtectDetector H1,On");
  EXPECT_TRUE(beam->detector("H1")->protect);
  ask(hook, "BlankBeam True");
  EXPECT_TRUE(beam->blanked());
}

TEST(WireBeam, QtegraTwoGetDataInARow) {
  ManualClock clock;
  auto beam = argon_beam(clock, {"H1"});
  auto model = std::make_shared<spectrometer::QtegraSimModel>();
  sim::feed_qtegra_from_beam(*model, beam);
  auto hook = spectrometer::qtegra_sim_hook(model);
  EXPECT_TRUE(q::decode_data(ask(hook, "GetData")));
  EXPECT_TRUE(q::decode_data(ask(hook, "GetData")));
}

namespace {

// The value of `channel` (index in channel order) on the first ACQ line due.
double first_acq(const SimTransport::Unsolicited& events, std::size_t channel) {
  const std::string text = to_string(events());
  const auto start = text.find("#EVENT:ACQ,");
  EXPECT_NE(start, std::string::npos) << text;
  if (start == std::string::npos) return 0.0;
  const auto end = text.find("\r\n", start);
  auto frame = codec::ngx::decode_acq_event(to_bytes(text.substr(start, end + 2 - start)));
  EXPECT_TRUE(frame) << text;
  if (!frame || channel >= frame->values.size()) return 0.0;
  return frame->values[channel];  // decoded back into channel order
}

}  // namespace

TEST(WireBeam, NgxReadsThePeak) {
  ManualClock clock;
  auto beam = argon_beam(clock, {"H1", "AX"});
  auto model = std::make_shared<spectrometer::NgxSimModel>();
  model->clock = &clock;
  model->banner_pending = false;
  sim::feed_ngx_from_beam(*model, beam, {"H1", "AX"});
  auto hook = spectrometer::ngx_sim_hook(model);
  auto events = spectrometer::ngx_sim_events(model);
  const auto ar40 = beam->peak_center("AX", "Ar40");
  ASSERT_TRUE(ar40);
  // As the driver sends it: the mass, then the settle delay.
  ask(hook, "SetMass " + std::to_string(*ar40) + ",500");
  EXPECT_DOUBLE_EQ(beam->magnet(), std::stod(std::to_string(*ar40)));
  ask(hook, "StartAcq 1,on");
  clock.advance(1s);
  const double on = first_acq(events, 1);
  ask(hook, "SetMass 0.5");
  ask(hook, "StartAcq 1,off");
  clock.advance(1s);
  const double off = first_acq(events, 1);
  EXPECT_GT(on, 100 * std::max(off, 1.0)) << "on " << on << ", off " << off;
}

TEST(WireBeam, NgxIonEnergySetsHv) {
  ManualClock clock;
  auto beam = argon_beam(clock, {"AX"});
  auto model = std::make_shared<spectrometer::NgxSimModel>();
  sim::feed_ngx_from_beam(*model, beam, {"AX"});
  auto hook = spectrometer::ngx_sim_hook(model);
  ask(hook, "SSO IE, 4100");
  EXPECT_DOUBLE_EQ(beam->hv(), 4100.0);
}
