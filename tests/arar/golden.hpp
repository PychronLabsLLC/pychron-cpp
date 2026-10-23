// Test-only Ar-Ar golden case parsing (spec 9.4): the shared reader
// (tests/support/golden_json.hpp) plus the helpers that build Ar-Ar inputs
// (measured_of, units_of, constants_of).
#pragma once

#include <string>
#include <string_view>

#include "golden_json.hpp"
#include "pychron/arar/arar_types.hpp"

namespace pychron::reduction::golden {

// ---- Shared case parsing ----------------------------------------------------

// A {"v", "e"} object.
inline Measured measured_of(const Json& j) { return {j["v"].as_number(), j["e"].as_number()}; }

// "a" / "ka" / "Ma" / "Ga"; false for anything else.
inline bool units_of(std::string_view s, AgeUnits& out) {
  if (s == "a") out = AgeUnits::a;
  else if (s == "ka") out = AgeUnits::ka;
  else if (s == "Ma") out = AgeUnits::Ma;
  else if (s == "Ga") out = AgeUnits::Ga;
  else return false;
  return true;
}

// A case's "constants" object. Every key is read; an unknown key or value is
// a failure (ADD_FAILURE, returns false).
inline bool constants_of(const Json& k, ReductionConstants& rc, const std::string& name) {
  bool ok = true;
  for (const auto& [key, v] : k.as_object()) {
    if (key == "lambda_b") rc.lambda_b = measured_of(v);
    else if (key == "lambda_e") rc.lambda_e = measured_of(v);
    else if (key == "include_decay_error") rc.include_decay_error = v.as_bool();
    else if (key == "age_units") {
      if (!units_of(v.as_string(), rc.age_units)) {
        ADD_FAILURE() << name << ": unknown age_units " << v.string;
        ok = false;
      }
    } else if (key == "lambda_cl36") rc.lambda_cl36 = measured_of(v);
    else if (key == "lambda_ar37") rc.lambda_ar37 = measured_of(v);
    else if (key == "lambda_ar39") rc.lambda_ar39 = measured_of(v);
    else if (key == "atm4036") rc.atm4036 = measured_of(v);
    else if (key == "atm4038") rc.atm4038 = measured_of(v);
    else if (key == "fixed_k3739") rc.fixed_k3739 = measured_of(v);
    else if (key == "allow_negative_ca_correction") rc.allow_negative_ca_correction = v.as_bool();
    else if (key == "abundance_sensitivity") rc.abundance_sensitivity = v.as_number();
    else if (key == "use_irradiation_endtime") rc.use_irradiation_endtime = v.as_bool();
    else if (key == "k3739_mode") {
      if (v.as_string() == "Fixed") {
        rc.k3739_mode = K3739Mode::Fixed;
      } else if (v.as_string() != "Normal") {
        ADD_FAILURE() << name << ": unknown k3739_mode " << v.string;
        ok = false;
      }
    } else if (key == "cosmogenic") {
      if (!v.is_null()) {
        rc.cosmogenic = CosmogenicRatios{measured_of(v["solar3836"]), measured_of(v["cosmo3836"])};
      }
    } else {
      ADD_FAILURE() << name << ": unhandled constants key " << key;
      ok = false;
    }
  }
  return ok;
}

}  // namespace pychron::reduction::golden
