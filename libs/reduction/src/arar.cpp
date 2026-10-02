// Live Ar-Ar quantities (conditionals spec 5) on the double kernels (spec 8.2).
#include "pychron/reduction/arar.hpp"

#include "arar_kernels.hpp"

namespace pychron::reduction {

// Every kernel's double instantiation (spec 8.1), including those the live path
// does not call yet, so the double path always builds warning-free.
namespace kernels {
template std::array<double, 5> abundance_sensitivity<double>(const std::array<double, 5>&, double);
template double deadtime<double>(const double&, double, double);
template Cosmogenic<double> cosmogenic<double>(const double&, const double&, const double&,
                                               const double&);
}  // namespace kernels

std::map<std::string, double> compute_arar(const ArArIntensities& in, const ArArConstants& c) {
  std::map<std::string, double> out;
  const bool h36 = in.ar36.has_value(), h37 = in.ar37.has_value();
  const bool h38 = in.ar38.has_value(), h39 = in.ar39.has_value(), h40 = in.ar40.has_value();

  // E8 then E9-E11. An absent Ar37 / Ar39 enters as 0 (legacy live rule).
  const double a37 = in.ar37.value_or(0.0) * c.df37;
  const double a39 = in.ar39.value_or(0.0) * c.df39;
  const kernels::InterferenceRatios<double> r{c.k3739, c.k3839, c.ca3937, c.ca3837, c.ca3637};
  const double* fixed =
      kernels::select_fixed_k3739(c.analysis_fixed_k3739, c.k3739_mode, c.fixed_k3739);
  const kernels::Interference<double> ik =
      kernels::interference(a39, a37, r, fixed, c.allow_negative_ca_correction);

  // ca37 depends on Ar39 through K3739 (E9) or entirely (E10).
  const bool ca_ok = h37 && (h39 || (fixed == nullptr && c.k3739 == 0.0));
  if (ca_ok) {
    out["ca37"] = ik.ca37;
    out["ca36"] = ik.ca36;
    out["ca39"] = ik.ca39;
  }
  const double ca36 = ca_ok ? ik.ca36 : 0.0;
  const double ca38 = ca_ok ? ik.ca38 : 0.0;
  const double k39 = h39 ? ik.k39 : 0.0;
  const double k38 = h39 ? ik.k38 : 0.0;
  if (h39) out["k39"] = k39;

  // E12. Without chlorine m = 0 and Ar38, k38, ca38 do not enter.
  const LiveChlorine* cl = c.chlorine ? &*c.chlorine : nullptr;
  if (!h36 || (cl != nullptr && !h38)) return out;
  const kernels::Atmospheric<double> atm =
      cl != nullptr ? kernels::atmospheric(*in.ar38, *in.ar36, k38, ca38, ca36, cl->decay_days,
                                           cl->cl3638, cl->lambda_cl36, c.atm4036 / cl->atm4038)
                    : kernels::atmospheric(0.0, *in.ar36, 0.0, 0.0, ca36, 0.0, 0.0, 0.0, 0.0);
  if (atm.singular) return out;

  // E14-E16.
  const kernels::FValues<double> fv =
      kernels::f_and_yield(in.ar40.value_or(0.0), k39, atm.atm36, c.atm4036, c.k4039);
  out["atm40"] = fv.atm40;
  if (h40) {
    out["rad40"] = fv.rad40;
    if (fv.yield_defined) out["radiogenic_yield"] = out["rad40_percent"] = fv.yield;
    if (fv.f_defined && c.j > 0 && c.lambda_total > 0) {
      const kernels::Age<double> a = kernels::age(c.j, fv.f, c.lambda_total, 1e-6);
      if (a.defined) out["age"] = a.age;
    }
  }

  // E19.
  if (h39 && ca_ok && ik.ca37 != 0.0 && k39 != 0.0) {
    const double kca = k39 / ik.ca37 * c.kca_factor;
    out["kca"] = kca;
    if (kca != 0.0) out["cak"] = 1.0 / kca;
  }
  if (cl != nullptr) {
    out["cl36"] = atm.cl36;
    if (h39 && atm.cl38 != 0.0 && k39 != 0.0) {
      const double kcl = k39 / atm.cl38 * cl->cl_k_factor;
      out["kcl"] = kcl;
      if (kcl != 0.0) out["clk"] = 1.0 / kcl;
    }
  }
  return out;
}

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
