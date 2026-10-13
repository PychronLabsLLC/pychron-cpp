// A hand-built level for the flux tests: the monitors of the flux golden data
// (three analyses each, F chosen so the arithmetic mean J is the golden J) and
// four unknowns at the golden prediction points.
#pragma once

#include <cmath>
#include <string>
#include <vector>

#include "../reduction/flux_golden.hpp"
#include "pychron/processing/flux_fit.hpp"

namespace pychron::processing::flux_test {

namespace pr = pychron::reduction;

inline MonitorSet fc2() {
  MonitorSet s;
  s.name = "FC-2";
  s.sample = "FC-2";
  s.age_ma = 28.201;
  s.lambda_ec = {5.757e-11, 1.6e-13};
  s.lambda_b = {4.955e-10, 1.34e-12};
  return s;
}

// J = (exp(lambda t) - 1) / F, so F = (exp(lambda t) - 1) / J.
inline LevelAnalysis analysis(const MonitorSet& set, int hole, int k, double j, const std::string& tag = "ok") {
  const auto c = set.constants();
  const double f = (std::exp(c.lambda_k * c.age_a) - 1.0) / j;
  LevelAnalysis a;
  a.uuid = "u-" + std::to_string(hole) + "-" + std::to_string(k);
  a.record_id = "M" + std::to_string(hole) + "-0" + std::to_string(k);
  a.tag = tag;
  a.f = pr::UFloat::variable(f, f * 0.01);
  return a;
}

// Three analyses with J, J(1 + d), J(1 - d): their arithmetic mean is J.
inline std::vector<LevelAnalysis> three(const MonitorSet& set, int hole, double j) {
  const double d = 2e-3;
  return {analysis(set, hole, 1, j), analysis(set, hole, 2, j * (1 + d)), analysis(set, hole, 3, j * (1 - d))};
}

// `ring` is flux_golden::kRing (default) or kMixed: eight monitors.
inline LevelInputs level(const flux_golden::MonitorRow* ring = flux_golden::kRing) {
  LevelInputs in;
  in.irradiation = "NM-300";
  in.level = "A";
  in.holder = "24-hole";
  in.monitor_set = fc2();
  int hole = 1;
  for (const auto* mp = ring; mp != ring + 8; ++mp) {
    const auto& m = *mp;
    LevelPosition p;
    p.hole = hole;
    p.position_uuid = "pos-" + std::to_string(hole);
    p.identifier = "6" + std::to_string(hole);
    p.sample = "FC-2";
    p.x = m.x;
    p.y = m.y;
    p.monitor = true;
    p.analyses = three(in.monitor_set, hole, m.j);
    in.positions.push_back(std::move(p));
    ++hole;
  }
  hole = 101;
  for (const auto& u : flux_golden::kPoints) {
    LevelPosition p;
    p.hole = hole;
    p.position_uuid = "pos-" + std::to_string(hole);
    p.identifier = "7" + std::to_string(hole);
    p.sample = "unk";
    p.x = u.x;
    p.y = u.y;
    in.positions.push_back(std::move(p));
    ++hole;
  }
  return in;
}

}  // namespace pychron::processing::flux_test
