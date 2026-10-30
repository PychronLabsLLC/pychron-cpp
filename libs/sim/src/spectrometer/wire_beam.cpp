#include "pychron/sim/spectrometer/wire_beam.hpp"

#include <chrono>
#include <map>
#include <optional>
#include <utility>

#include "pychron/codecs/codec.hpp"
#include "pychron/codecs/isotopx_ngx.hpp"
#include "pychron/codecs/thermo_qtegra.hpp"
#include "pychron/devices/spectrometer/params.hpp"

namespace pychron::sim {

namespace {

namespace q = codec::qtegra;

std::optional<double> number(const std::string& text) { return codec::parse_decimal(text); }

Duration seconds(double s) {
  return std::chrono::duration_cast<Duration>(std::chrono::duration<double>(s > 0 ? s : 1.0));
}

// What a Qtegra command does to the beam; commands that set nothing do nothing.
void follow_qtegra(BeamModel& beam, const std::string& verb, const std::vector<std::string>& args) {
  if ((verb == "SetMagnetDAC" || verb == "SetHV") && args.size() == 1) {
    if (const auto v = number(args[0])) (verb == "SetHV") ? beam.set_hv(*v) : beam.set_magnet(*v);
  } else if ((verb == "SetDeflection" || verb == "SetGain") && args.size() == 2) {
    if (const auto v = number(args[1])) {
      (void)(verb == "SetDeflection" ? beam.set_deflection(args[0], *v) : beam.set_gain(args[0], *v));
    }
  } else if (verb == "ProtectDetector" && args.size() == 2) {
    (void)beam.protect(args[0], args[1] == "On");
  } else if (verb == "BlankBeam" && args.size() == 1) {
    beam.blank(args[0] == "true");
  } else if (verb == "SetParameter" && args.size() == 2) {
    const auto v = number(args[1]);
    const auto canonical = q::canonical_name(args[0]);
    if (!v || !canonical) return;
    if (auto id = spectrometer::parse_param_id(*canonical)) beam.set_source_param(*id, *v);
  }
}

void follow_ngx(BeamModel& beam, const std::string& verb, const std::string& args) {
  if (verb == "SetMass") {
    // "SetMass 39.962,500": the mass, then the settle delay (ms).
    if (const auto v = number(args.substr(0, args.find(',')))) beam.set_magnet(*v);
  } else if (verb == "SSO") {
    // "SSO IE, 4500": a mnemonic, a comma, the value.
    const auto comma = args.find(',');
    if (comma == std::string::npos) return;
    std::string value = args.substr(comma + 1);
    while (!value.empty() && value.front() == ' ') value.erase(0, 1);
    const auto v = number(value);
    const auto param = codec::ngx::param_from_mnemonic(args.substr(0, comma));
    if (!v || !param) return;
    if (*param == codec::ngx::Param::IonEnergy) {
      beam.set_hv(*v);
    } else if (auto id = spectrometer::parse_param_id(codec::ngx::name(*param))) {
      beam.set_source_param(*id, *v);
    } else {
      beam.set_source_param(spectrometer::Custom{std::string(codec::ngx::name(*param))}, *v);
    }
  }
}

}  // namespace

void feed_qtegra_from_beam(spectrometer::QtegraSimModel& model, const std::shared_ptr<BeamModel>& beam) {
  beam->set_hv(beam->nominal_hv());
  model.hv = beam->nominal_hv();
  model.detectors = beam->detector_names();
  model.data_source = [beam](double integration_s) {
    std::map<std::string, double> data;
    for (const auto& name : beam->detector_names()) {
      if (auto reading = beam->intensity(name, seconds(integration_s))) data.emplace(name, reading->value);
    }
    return data;
  };
  model.on_command = [beam, before = std::move(model.on_command)](const std::string& verb,
                                                                   const std::vector<std::string>& args) {
    if (before) before(verb, args);
    follow_qtegra(*beam, verb, args);
  };
}

void feed_ngx_from_beam(spectrometer::NgxSimModel& model, const std::shared_ptr<BeamModel>& beam,
                        std::vector<std::string> channels) {
  model.values_at = [beam, channels = std::move(channels)](TimePoint at) {
    std::vector<double> values;
    values.reserve(channels.size());
    for (const auto& c : channels) {
      auto reading = beam->intensity(c, at, std::chrono::seconds(1));
      values.push_back(reading ? reading->value : 0.0);
    }
    return values;
  };
  model.on_command = [beam, before = std::move(model.on_command)](const std::string& verb, const std::string& args) {
    if (before) before(verb, args);
    follow_ngx(*beam, verb, args);
  };
}

}  // namespace pychron::sim
