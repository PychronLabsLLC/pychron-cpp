#include <gtest/gtest.h>

#include <random>

#include "pychron/systems/spectrometer/corrections.hpp"

using namespace pychron;
using namespace pychron::spectrometer;
using Axis = IMassPositioner::Axis;

namespace {

CorrectionInputs full(Axis axis = Axis::Dac) {
  CorrectionInputs in;
  in.axis = axis;
  in.deflection_enabled = true;
  in.deflection_poly = {0.01, 0.0012, 1e-7};
  in.deflection_sign = -1;
  in.deflection = 150.0;
  in.hv_enabled = true;
  in.hv_actual = 4400.0;
  in.hv_nominal = 4500.0;
  return in;
}

}  // namespace

TEST(Corrections, DisabledIsIdentity) {
  CorrectionInputs in;
  EXPECT_DOUBLE_EQ(*correct(5.001, in), 5.001);
  EXPECT_DOUBLE_EQ(*uncorrect(5.001, in), 5.001);
}

TEST(Corrections, DeflectionThenHvInFixedOrder) {
  auto in = full();
  const double offset = -1.0 * (0.01 + 0.0012 * 150.0 + 1e-7 * 150.0 * 150.0);
  EXPECT_DOUBLE_EQ(deflection_offset(in), offset);
  EXPECT_NEAR(*correct(5.0, in), (5.0 + offset) * std::sqrt(4400.0 / 4500.0), 1e-12);
}

TEST(Corrections, HvCorrectionOnlyForDacAxis) {
  for (Axis axis : {Axis::Field, Axis::Mass}) {
    auto in = full(axis);
    EXPECT_DOUBLE_EQ(*hv_factor(in), 1.0);
    EXPECT_NEAR(*correct(5.0, in), 5.0 + deflection_offset(in), 1e-12);
  }
}

TEST(Corrections, NonPositiveHvIsConfigError) {
  auto in = full();
  in.hv_actual = 0.0;
  auto r = correct(5.0, in);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
}

TEST(Corrections, UncorrectIsExactInverseAcrossAxes) {
  std::mt19937_64 rng(42);
  std::uniform_real_distribution<double> value(0.0, 10.0), defl(-800.0, 800.0), hv(3000.0, 5000.0);
  for (Axis axis : {Axis::Dac, Axis::Field, Axis::Mass}) {
    for (int i = 0; i < 500; ++i) {
      auto in = full(axis);
      in.deflection = defl(rng);
      in.hv_actual = hv(rng);
      in.deflection_enabled = (i % 3) != 0;
      in.hv_enabled = (i % 2) != 0;
      const double v = value(rng);
      auto native = correct(v, in);
      ASSERT_TRUE(native.has_value());
      auto back = uncorrect(*native, in);
      ASSERT_TRUE(back.has_value());
      EXPECT_NEAR(*back, v, 1e-12);
    }
  }
}
