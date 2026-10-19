#pragma once

// In-memory inputs of the row tables, for the publish tests that need no
// store: three steps of one aliquot of sample FC-2 and one fusion of sample
// "U1 sample", reduced and grouped by aliquot as the report groups them.

#include <array>
#include <string>

#include "processing/fixtures.hpp"
#include "pychron/publish/tables.hpp"

namespace pychron::publish::testing {

namespace pp = pychron::processing;
namespace ps = pychron::persistence;
using pp::test::make_step;
using pp::test::make_unknown;

// Fixed ids, so two builds of the same inputs are the same bytes.
inline const ps::Uuid kNamespace = *ps::Uuid::parse("0192f3a0-0000-7000-8000-000000000001");

inline ps::SampleRow sample_row(const std::string& name, const std::string& uuid_seed, bool located) {
  ps::SampleRow s;
  s.uuid = ps::Uuid::v5(kNamespace, uuid_seed);
  s.name = name;
  s.project = ps::Uuid::v5(kNamespace, "project");
  s.material = ps::Uuid::v5(kNamespace, "material");
  s.project_name = "Fish Canyon";
  s.principal_investigator_name = "J Ross";
  s.material_name = "sanidine";
  s.grainsize = "60-80";
  if (located) {
    s.fields.lat = 37.75;
    s.fields.lon = -106.9;
    s.fields.elevation = 2850.0;
  }
  s.fields.igsn = "IEFC20001";
  s.updated = *ps::UtcTime::parse("2026-10-18T00:00:00Z");
  return s;
}

// Three steps of one aliquot of sample FC-2 and one fusion of sample U1,
// reduced and grouped by aliquot as the report groups them.
inline TableInputs inputs(bool fc2_located = true, bool u1_has_age = true) {
  TableInputs in;
  in.release.uuid = ps::Uuid::v5(kNamespace, "release");
  in.release.name = "r";
  in.release.version = 1;
  in.release.options_json = "{}";
  const ps::SampleRow fc2 = sample_row("FC-2", "fc2", fc2_located);
  const ps::SampleRow u1 = sample_row("U1 sample", "u1", true);
  in.samples = {u1, fc2};  // not in name order on purpose
  pp::Dataset d;
  const std::array<double, 3> ar39{20, 40, 60};
  const std::array<double, 3> ar36{0.3, 0.2, 0.1};
  for (int i = 0; i < 3; ++i) {
    auto a = make_step(i, ar39[i], ar36[i]);
    a->material = "sanidine";
    a->project = "Fish Canyon";
    a->irradiation = "NM-300";
    a->level = "A";
    a->position = "3";
    a->sample_info.latitude = fc2.fields.lat;
    a->sample_info.longitude = fc2.fields.lon;
    AnalysisPins pins;
    pins.analysis = ps::Uuid::v5(in.release.uuid, a->uuid);
    a->uuid = pins.analysis.str();  // as the store's source names an analysis
    pp::DatasetItem item;
    item.analysis = pp::reduce_analysis(a, {});
    item.path.group = 0;
    d.mutable_items().push_back(item);
    pins.sample = fc2.uuid;
    pins.heads[ps::Kind::Intercepts] = ps::Uuid::v5(pins.analysis, "intercepts");
    pins.heads[ps::Kind::Tags] = ps::Uuid::v5(pins.analysis, "tags");
    in.pins.push_back(pins);
  }
  {
    auto a = make_unknown(0);
    a->sample = "U1 sample";
    a->material = "sanidine";
    a->project = "Fish Canyon";
    a->timestamp = 1'700'000'000.0 - 600.0;  // before every step: it sorts first
    if (!u1_has_age) a->context.flux = std::nullopt;
    AnalysisPins pins;
    pins.analysis = ps::Uuid::v5(in.release.uuid, a->uuid);
    a->uuid = pins.analysis.str();
    pp::DatasetItem item;
    item.analysis = pp::reduce_analysis(a, {});
    item.path.group = 1;
    d.mutable_items().push_back(item);
    pins.sample = u1.uuid;
    in.pins.push_back(pins);
  }
  d.group_names = {"S1-01", "U1-01"};
  d.reduction_tag = "arar-1/default/d0";
  in.dataset = d;
  in.report = pp::make_report(d, {});
  return in;
}


}  // namespace pychron::publish::testing
