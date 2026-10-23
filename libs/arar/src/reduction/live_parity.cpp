// Live constants from reduce()'s inputs (spec 8.2).
#include "pychron/arar/live_parity.hpp"

namespace pychron::reduction {

namespace {

double factor_of(const std::optional<Measured>& ratio) {
  return ratio && ratio->value != 0.0 ? 1.0 / ratio->value : 1.0;
}

}  // namespace

ArArConstants to_live_constants(const ReductionConstants& c, const ProductionRatios& p,
                                const Flux& flux, const DecayFactors& df,
                                std::optional<double> decay_days) {
  ArArConstants out;
  // E16 lambda_K as reduce() resolves it (truthy override, else b + e).
  const bool override_lk = flux.lambda_k_total && !(flux.lambda_k_total->value == 0.0 &&
                                                    flux.lambda_k_total->error == 0.0);
  out.lambda_total =
      override_lk ? flux.lambda_k_total->value : c.lambda_b.value + c.lambda_e.value;
  out.atm4036 = c.atm4036.value;
  out.ca3637 = p.ca3637.value;
  out.ca3937 = p.ca3937.value;
  out.k4039 = p.k4039.value;
  out.k3739 = p.k3739.value;
  out.k3839 = p.k3839.value;
  out.ca3837 = p.ca3837.value;
  out.j = flux.j.value;
  out.df37 = df.df37;
  out.df39 = df.df39;
  out.kca_factor = factor_of(p.ca_k);
  out.allow_negative_ca_correction = c.allow_negative_ca_correction;
  out.k3739_mode = c.k3739_mode;
  out.fixed_k3739 = c.fixed_k3739.value;
  if (decay_days) {
    LiveChlorine cl;
    cl.cl3638 = p.cl3638.value;
    cl.lambda_cl36 = c.lambda_cl36.value;
    cl.decay_days = *decay_days;
    cl.atm4038 = c.atm4038.value;
    cl.cl_k_factor = factor_of(p.cl_k);
    out.chlorine = cl;
  }
  return out;
}

}  // namespace pychron::reduction
