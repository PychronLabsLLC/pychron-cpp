#pragma once

// LinearFlow: the exact solution of dn/dt = K n + s for one species over N
// volumes, K and s constant (spec sections 3.3 and 3.4).
//
// n_i is the amount of the species in volume i (mbar L), p_i = n_i / V_i its
// partial pressure. Between valve events the amounts obey
//
//   dn_i/dt = sum_j C_ij (n_j / V_j - n_i / V_i)     links between i and j
//             - loss_i n_i                           pumps and first-order loss
//             + source_i                             constant inflow
//
// which is linear with constant coefficients, so it has a closed form and no
// step size. K is not symmetric, but scaling row and column i by sqrt(V_i)
// makes it so: with D = diag(sqrt(V)), A = D^-1 K D has
//
//   A_ii = -(sum of conductances on i) / V_i - loss_i
//   A_ij = C_ij / sqrt(V_i V_j)
//
// A is decomposed once, A = U L U^T, by cyclic Jacobi rotation (N is tens at
// most), and every eigenvalue is <= 0. In y = U^T D^-1 n the equations come
// apart, dy_k/dt = l_k y_k + g_k with g = U^T D^-1 s, and each is advanced by
// any dt at once:
//
//   y_k(t + dt) = y_k(t) exp(l_k dt) + g_k (exp(l_k dt) - 1) / l_k
//
// where the last factor is dt when l_k is zero. A zero eigenvalue is a group
// of linked volumes that loses nothing: its mode is the amount the group
// holds, which a source grows linearly and nothing else changes.
//
// Two things keep that exact where rates differ by many orders (a valve that
// equilibrates in a microsecond beside a loss of hours), and both are in how
// the decomposition is made, not in what it is:
//   - the mode of a group that loses nothing is known beforehand (sqrt(V)
//     over the group) and is taken out exactly, never computed: what such a
//     group holds does not drift, whatever else is in the system;
//   - the rotations are applied to a factor G of -A = G^T G (one row per
//     link and per loss) rather than to A, so a slow rate is found to
//     rounding of itself, not of the fastest rate in the system.
//
// Consequences the rest of the simulator leans on:
//   - stiffness does not matter: a valve that equilibrates in a millisecond
//     and an outgassing rate of hours coexist, at any dt;
//   - one step of an hour equals 3600 steps of a second to rounding, so the
//     answer does not depend on how often anything asks;
//   - amounts stay finite and never go below zero.
//
// Knows no clock and holds no state but the decomposition: the caller owns n.
// Immutable once made, so `advance` may be called from any thread.

#include <cstddef>
#include <vector>

#include "pychron/core/error.hpp"

namespace pychron::sim {

// K and s by their parts, so that K is symmetric in the sqrt(V) scaling by
// construction. `loss` and `source` have one entry per volume.
struct FlowTerms {
  std::vector<double> volume;  // litres, > 0
  struct Link {
    std::size_t a, b;
    double conductance;  // L/s between volumes a and b
  };
  // A closed valve is a link of conductance 0, or no link; links between the
  // same two volumes add.
  std::vector<Link> links;
  std::vector<double> loss;    // 1/s per volume: pump speed / V plus first-order loss
  std::vector<double> source;  // mbar L / s per volume: constant inflow plus pump base * speed
};

class LinearFlow {
 public:
  // Config error on a bad size, a link to a volume that is not there or to
  // itself, or a non-finite or negative value (a volume must be > 0).
  static Result<LinearFlow> make(FlowTerms terms);

  // n(t + dt) from n(t); n in mbar L, one entry per volume. dt >= 0, and a dt
  // that is not (zero, negative, NaN) leaves n alone. Does not allocate for
  // 32 volumes or fewer.
  void advance(std::vector<double>& n, double dt) const;

  std::size_t size() const noexcept { return size_; }

 private:
  LinearFlow() = default;

  std::size_t size_ = 0;
  std::vector<double> rate_;       // l_k, <= 0; exactly 0 for a mode that loses nothing
  std::vector<double> drive_;      // g = U^T D^-1 s
  std::vector<double> to_mode_;    // U^T D^-1, row-major: y = to_mode_ n
  std::vector<double> to_amount_;  // D U, row-major: n = to_amount_ y
};

}  // namespace pychron::sim
