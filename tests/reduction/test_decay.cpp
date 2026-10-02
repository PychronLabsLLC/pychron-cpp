#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <tuple>
#include <vector>

#include "golden.hpp"
#include "pychron/reduction/arar_reduction.hpp"

using namespace pychron::reduction;
namespace g = pychron::reduction::golden;

namespace {
std::string fmt(const char* what, double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%g", v);
  return std::string(what) + buf;
}

std::vector<DecaySegment> segments_of(const g::Json& arr) {
  std::vector<DecaySegment> out;
  for (std::size_t i = 0; i < arr.size(); ++i) {
    out.push_back({arr[i]["power"].as_number(), arr[i]["duration_days"].as_number(),
                   arr[i]["dt_days"].as_number()});
  }
  return out;
}
}  // namespace

TEST(Decay, NoSegmentsIsUnity) {
  auto r = decay_factors(1e-10, 1e-10, {});
  ASSERT_TRUE(r) << to_string(r.error());
  EXPECT_EQ(r->df37, 1.0);
  EXPECT_EQ(r->df39, 1.0);
}

TEST(Decay, KnownSingleSegment) {
  const std::vector<DecaySegment> s{{1.0, 86400.0, 31536000.0}};
  auto r = decay_factors(7.2e-10, 7e-10, s);
  ASSERT_TRUE(r) << to_string(r.error());
  EXPECT_NEAR(r->df37, 1.022997480219743, 1.022997480219743 * 1e-13);
  EXPECT_NEAR(r->df39, 1.0223515753822912, 1.0223515753822912 * 1e-13);
}

TEST(Decay, UnitGuardErrors) {
  auto bad = decay_factors(0.02, 7e-6, std::vector<DecaySegment>{{1.0, 86400.0, 31536000.0}});
  ASSERT_FALSE(bad);
  EXPECT_EQ(bad.error().kind, pychron::ErrorKind::Config);
  EXPECT_NE(bad.error().what.find("reduction: "), std::string::npos);
  EXPECT_NE(bad.error().what.find("same unit"), std::string::npos);
  EXPECT_TRUE(decay_factors(1.0, 1.0, std::vector<DecaySegment>{{1.0, 50.0, 1.0}}));
  EXPECT_FALSE(decay_factors(1.0, 1.0, std::vector<DecaySegment>{{1.0, 50.0001, 1.0}}));
}

TEST(Decay, ZeroLambdaWithSegmentsErrors) {
  const std::vector<DecaySegment> seg{{1.0, 1.0, 10.0}};
  for (const auto& [l37, l39, field] :
       {std::tuple{0.0, 7e-6, "lambda_ar37"}, std::tuple{0.01975, 0.0, "lambda_ar39"}}) {
    auto r = decay_factors(l37, l39, seg);
    if (r) {
      ADD_FAILURE() << field << ": expected an error";
      continue;
    }
    EXPECT_EQ(r.error().kind, pychron::ErrorKind::Config);
    EXPECT_TRUE(r.error().what.starts_with("reduction: ")) << r.error().what;
    EXPECT_NE(r.error().what.find(field), std::string::npos) << r.error().what;
  }
  // Without segments the lambdas are not read.
  EXPECT_TRUE(decay_factors(0.0, 0.0, std::vector<DecaySegment>{}));
}

TEST(Decay, ZeroDenominatorIsUnity) {
  auto r = decay_factors(0.01975, 7e-6, std::vector<DecaySegment>{{0.0, 1.0, 1.0}});
  ASSERT_TRUE(r) << to_string(r.error());
  EXPECT_EQ(r->df37, 1.0);
  EXPECT_EQ(r->df39, 1.0);
}

TEST(Decay, SegmentsFromDosesStartVsEnd) {
  const std::vector<Dose> doses{{1.0, 0, 43200}, {0.5, 86400, 172800}};
  const std::int64_t now = 259200;  // 3 days after first start
  auto a = irradiation_from_doses(doses, now, false);
  ASSERT_EQ(a.segments.size(), 2u);
  EXPECT_DOUBLE_EQ(a.decay_days, 3.0);
  EXPECT_DOUBLE_EQ(a.segments[0].duration_days, 0.5);
  EXPECT_DOUBLE_EQ(a.segments[1].duration_days, 1.0);
  EXPECT_DOUBLE_EQ(a.segments[0].power, 1.0);
  EXPECT_DOUBLE_EQ(a.segments[1].power, 0.5);
  EXPECT_DOUBLE_EQ(a.segments[0].dt_days, 3.0);
  EXPECT_DOUBLE_EQ(a.segments[1].dt_days, 2.0);
  auto b = irradiation_from_doses(doses, now, true);
  EXPECT_DOUBLE_EQ(b.decay_days, 3.0);
  EXPECT_DOUBLE_EQ(b.segments[0].dt_days, 2.5);
  EXPECT_DOUBLE_EQ(b.segments[1].dt_days, 1.0);
  EXPECT_TRUE(irradiation_from_doses({}, now, false).segments.empty());
}

TEST(Decay, Golden) {
  const g::Json doc = g::load("decay_factors.json");
  const g::Json& cases = doc["cases"];
  ASSERT_EQ(cases.size(), 18u);
  std::size_t n_decay = 0, n_chron = 0;
  for (std::size_t i = 0; i < cases.size(); ++i) {
    const g::Json& c = cases[i];
    const std::string name = c["name"].string;
    SCOPED_TRACE(name);
    if (!c["legacy_sentinel"].is_null()) ADD_FAILURE() << name << ": unhandled legacy_sentinel";
    if (c["expect_diagnostics"].size() != 0) ADD_FAILURE() << name << ": unhandled expect_diagnostics";
    const g::Tol t = g::tol_of(c);
    const g::Json& in = c["inputs"];
    const g::Json& want = c["expected"];
    const double l37 = in["lambda37"].as_number(), l39 = in["lambda39"].as_number();
    const std::string fn = in["function"].string;
    std::vector<DecaySegment> segs;
    if (fn == "decay_factors") {
      ++n_decay;
      segs = segments_of(in["segments"]);
    } else if (fn == "irradiation_from_doses") {
      ++n_chron;
      std::vector<Dose> doses;
      const g::Json& d = in["doses"];
      for (std::size_t k = 0; k < d.size(); ++k) {
        doses.push_back({d[k]["power"].as_number(),
                         static_cast<std::int64_t>(d[k]["start_utc_s"].as_number()),
                         static_cast<std::int64_t>(d[k]["end_utc_s"].as_number())});
      }
      const Irradiation irr =
          irradiation_from_doses(doses, static_cast<std::int64_t>(in["analysis_utc_s"].as_number()),
                                 in["use_irradiation_endtime"].boolean);
      g::expect_close(irr.decay_days, want["decay_days"].as_number(), t.rtol, t.atol, "decay_days");
      const g::Json& ws = want["segments"];
      if (irr.segments.size() != ws.size()) {
        ADD_FAILURE() << fmt("segment count ", static_cast<double>(irr.segments.size()));
        continue;
      }
      for (std::size_t k = 0; k < ws.size(); ++k) {
        g::expect_close(irr.segments[k].dt_days, ws[k]["dt_days"].as_number(), t.rtol, t.atol, "dt_days");
        g::expect_close(irr.segments[k].duration_days, ws[k]["duration_days"].as_number(), t.rtol,
                        t.atol, "duration_days");
        g::expect_close(irr.segments[k].power, ws[k]["power"].as_number(), t.rtol, t.atol, "power");
      }
      segs = irr.segments;
    } else {
      ADD_FAILURE() << name << ": unknown function " << fn;
      continue;
    }
    auto r = decay_factors(l37, l39, segs);
    if (c["expect_error"].is_string()) {
      if (r) {
        ADD_FAILURE() << name << ": expected error";
      } else {
        EXPECT_NE(r.error().what.find(c["expect_error"].string), std::string::npos) << r.error().what;
      }
      continue;
    }
    if (!r) {
      ADD_FAILURE() << name << ": " << r.error().what;
      continue;
    }
    g::expect_close(r->df37, want["df37"].as_number(), t.rtol, t.atol, "df37");
    g::expect_close(r->df39, want["df39"].as_number(), t.rtol, t.atol, "df39");
  }
  EXPECT_EQ(n_decay, 12u);
  EXPECT_EQ(n_chron, 6u);
}
