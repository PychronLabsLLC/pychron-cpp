#include "pychron/sim/linear_flow.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "pychron/core/error.hpp"

namespace pychron::sim {
namespace {

// Volumes whose modes fit on the stack in `advance`.
constexpr std::size_t kInlineModes = 32;

// x (rows x n) times y (n x n), both row-major.
std::vector<double> product(const std::vector<double>& x, std::size_t rows, const std::vector<double>& y,
                            std::size_t n) {
  std::vector<double> out(rows * n, 0.0);
  for (std::size_t i = 0; i < rows; ++i) {
    for (std::size_t k = 0; k < n; ++k) {
      const double xik = x[i * n + k];
      if (xik == 0.0) continue;
      for (std::size_t j = 0; j < n; ++j) out[i * n + j] += xik * y[k * n + j];
    }
  }
  return out;
}

// Cyclic Jacobi rotation, in its one-sided form (Hestenes): the columns of
// `g` (rows x n, row-major) are rotated in pairs until they are orthogonal
// to each other. With every rotation gathered into `u`, the columns of `u`
// are then the eigenvectors of g^T g and the squared lengths of the columns
// of `g` its eigenvalues.
//
// Rotating the factor rather than the product is what keeps a slow rate
// right beside a fast one. A rotation of g^T g itself rounds at the size of
// its largest entry, so a rate 1e-12 of the fastest would be lost whole;
// here each pair is judged, and rounds, against its own two columns.
void rotate_to_orthogonal_columns(std::vector<double>& g, std::size_t rows, std::vector<double>& u,
                                  std::size_t n) {
  // Columns closer to orthogonal than this are left: it is what a product
  // of two columns rounds to.
  constexpr double kOrthogonal = 1e-15;
  constexpr int kMaxSweeps = 100;
  for (int sweep = 0; sweep < kMaxSweeps; ++sweep) {
    bool rotated = false;
    for (std::size_t p = 0; p < n; ++p) {
      for (std::size_t q = p + 1; q < n; ++q) {
        double pp = 0.0;
        double qq = 0.0;
        double pq = 0.0;
        for (std::size_t r = 0; r < rows; ++r) {
          const double gp = g[r * n + p];
          const double gq = g[r * n + q];
          pp += gp * gp;
          qq += gq * gq;
          pq += gp * gq;
        }
        if (std::abs(pq) <= kOrthogonal * std::sqrt(pp) * std::sqrt(qq)) continue;
        rotated = true;
        // The rotation that makes the pair orthogonal, by its smaller
        // angle: t is its tangent.
        const double theta = (qq - pp) / (2.0 * pq);
        const double t = (theta < 0.0 ? -1.0 : 1.0) / (std::abs(theta) + std::hypot(theta, 1.0));
        const double c = 1.0 / std::hypot(t, 1.0);
        const double s = t * c;
        for (std::size_t r = 0; r < rows; ++r) {
          const double gp = g[r * n + p];
          const double gq = g[r * n + q];
          g[r * n + p] = c * gp - s * gq;
          g[r * n + q] = s * gp + c * gq;
        }
        for (std::size_t r = 0; r < n; ++r) {
          const double up = u[r * n + p];
          const double uq = u[r * n + q];
          u[r * n + p] = c * up - s * uq;
          u[r * n + q] = s * up + c * uq;
        }
      }
    }
    if (!rotated) return;
  }
}

bool amount(double value) { return std::isfinite(value) && value >= 0.0; }

}  // namespace

Result<LinearFlow> LinearFlow::make(FlowTerms terms) {
  const std::size_t n = terms.volume.size();
  if (terms.loss.size() != n || terms.source.size() != n) {
    return fail(ErrorKind::Config, "linear flow: " + std::to_string(n) + " volumes, " +
                                       std::to_string(terms.loss.size()) + " losses and " +
                                       std::to_string(terms.source.size()) + " sources");
  }
  for (std::size_t i = 0; i < n; ++i) {
    const std::string where = "linear flow: volume " + std::to_string(i);
    if (!(std::isfinite(terms.volume[i]) && terms.volume[i] > 0.0)) {
      return fail(ErrorKind::Config, where + " is not a size above zero");
    }
    if (!amount(terms.loss[i])) return fail(ErrorKind::Config, where + " has a negative or non-finite loss");
    if (!amount(terms.source[i])) return fail(ErrorKind::Config, where + " has a negative or non-finite source");
  }
  for (std::size_t k = 0; k < terms.links.size(); ++k) {
    const FlowTerms::Link& link = terms.links[k];
    const std::string where = "linear flow: link " + std::to_string(k);
    if (link.a >= n || link.b >= n) return fail(ErrorKind::Config, where + " joins a volume that is not there");
    if (link.a == link.b) return fail(ErrorKind::Config, where + " joins a volume to itself");
    if (!amount(link.conductance)) {
      return fail(ErrorKind::Config, where + " has a negative or non-finite conductance");
    }
  }

  std::vector<double> root(n);  // D
  for (std::size_t i = 0; i < n; ++i) root[i] = std::sqrt(terms.volume[i]);

  // -A = G^T G, one row of G per term: a link of conductance C between a and
  // b gives sqrt(C) (e_a / sqrt(V_a) - e_b / sqrt(V_b)), a loss L on i gives
  // sqrt(L) e_i. Squared and summed those are the entries of A in the
  // header; A itself is never formed.
  std::size_t rows = 0;
  for (const FlowTerms::Link& link : terms.links) rows += link.conductance > 0.0 ? 1 : 0;
  for (const double loss : terms.loss) rows += loss > 0.0 ? 1 : 0;
  std::vector<double> g(rows * n, 0.0);
  std::size_t row = 0;
  for (const FlowTerms::Link& link : terms.links) {
    if (!(link.conductance > 0.0)) continue;
    const double flow = std::sqrt(link.conductance);
    g[row * n + link.a] = flow / root[link.a];
    g[row * n + link.b] = -flow / root[link.b];
    ++row;
  }
  for (std::size_t i = 0; i < n; ++i) {
    if (!(terms.loss[i] > 0.0)) continue;
    g[row * n + i] = std::sqrt(terms.loss[i]);
    ++row;
  }

  // A group of linked volumes that loses nothing keeps what it holds, and
  // the eigenvector that says so is known without computing anything: it is
  // sqrt(V) over the group, with eigenvalue zero. Found by rotation it would
  // be right only to rounding, and gas would be made or lost at that rate on
  // every step; so those vectors are taken out exactly first. H is a
  // reflection, one block per group, whose column at the group's first
  // volume is that vector: G H has a column of zeros there, which no
  // rotation touches, and every other mode stays orthogonal to it.
  std::vector<std::size_t> group(n);
  for (std::size_t i = 0; i < n; ++i) group[i] = i;
  const auto first_of = [&group](std::size_t i) {
    while (group[i] != i) i = group[i] = group[group[i]];
    return i;
  };
  for (const FlowTerms::Link& link : terms.links) {
    if (!(link.conductance > 0.0)) continue;
    const std::size_t x = first_of(link.a);
    const std::size_t y = first_of(link.b);
    group[std::max(x, y)] = std::min(x, y);
  }
  std::vector<double> held(n, 0.0);        // litres in the group, at its first volume
  std::vector<char> closed(n, 1);          // the group loses nothing, likewise
  std::vector<std::size_t> members(n, 0);  // and how many volumes it has
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t first = first_of(i);
    held[first] += terms.volume[i];
    members[first] += 1;
    if (terms.loss[i] > 0.0) closed[first] = 0;
  }
  const auto conserved = [&](std::size_t i) { return group[i] == i && closed[i] != 0; };

  std::vector<double> h(n * n, 0.0);
  for (std::size_t i = 0; i < n; ++i) h[i * n + i] = 1.0;
  std::vector<double> v(n);
  for (std::size_t first = 0; first < n; ++first) {
    if (!conserved(first) || members[first] < 2) continue;  // alone, it is e_first already
    // v = w + e_first, w the unit vector along sqrt(V) over the group:
    // H = I - 2 v v^T / v.v takes e_first to -w.
    double vv = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      v[i] = first_of(i) == first ? root[i] / std::sqrt(held[first]) + (i == first ? 1.0 : 0.0) : 0.0;
      vv += v[i] * v[i];
    }
    for (std::size_t i = 0; i < n; ++i) {
      if (v[i] == 0.0) continue;
      for (std::size_t j = 0; j < n; ++j) h[i * n + j] -= 2.0 * v[i] * v[j] / vv;
    }
  }
  g = product(g, rows, h, n);
  for (std::size_t k = 0; k < n; ++k) {
    if (!conserved(k)) continue;
    for (std::size_t r = 0; r < rows; ++r) g[r * n + k] = 0.0;
  }

  std::vector<double> u = h;
  rotate_to_orthogonal_columns(g, rows, u, n);

  LinearFlow flow;
  flow.size_ = n;
  // Minus a squared length: never above zero, and exactly zero for what is
  // conserved. There is no rounding to tell from a slow rate, so a loss of
  // 1e-5 / s beside a valve of 1e7 / s is still a loss of 1e-5 / s.
  flow.rate_.assign(n, 0.0);
  for (std::size_t k = 0; k < n; ++k) {
    double length = 0.0;
    for (std::size_t r = 0; r < rows; ++r) length += g[r * n + k] * g[r * n + k];
    flow.rate_[k] = length > 0.0 ? -length : 0.0;
  }

  flow.to_mode_.resize(n * n);
  flow.to_amount_.resize(n * n);
  for (std::size_t i = 0; i < n; ++i) {
    for (std::size_t k = 0; k < n; ++k) {
      flow.to_mode_[k * n + i] = u[i * n + k] / root[i];
      flow.to_amount_[i * n + k] = root[i] * u[i * n + k];
    }
  }
  // The mode of a group that loses nothing is, to a factor, the plain sum of
  // what its volumes hold, shared out again by volume. Written as that, with
  // no square roots to round, the sum is kept to the last bit or two, and a
  // volume joined to nothing is not touched at all.
  for (std::size_t first = 0; first < n; ++first) {
    if (!conserved(first)) continue;
    for (std::size_t i = 0; i < n; ++i) {
      const bool member = first_of(i) == first;
      flow.to_mode_[first * n + i] = member ? 1.0 : 0.0;
      flow.to_amount_[i * n + first] = member ? terms.volume[i] / held[first] : 0.0;
    }
  }
  flow.drive_.assign(n, 0.0);
  for (std::size_t k = 0; k < n; ++k) {
    for (std::size_t i = 0; i < n; ++i) flow.drive_[k] += flow.to_mode_[k * n + i] * terms.source[i];
  }
  return flow;
}

void LinearFlow::advance(std::vector<double>& n, double dt) const {
  assert(n.size() == size_);
  if (n.size() != size_ || !(dt > 0.0)) return;

  std::array<double, kInlineModes> inline_modes;
  std::vector<double> more_modes;
  double* y = inline_modes.data();
  if (size_ > kInlineModes) {
    more_modes.resize(size_);
    y = more_modes.data();
  }

  for (std::size_t k = 0; k < size_; ++k) {
    double mode = 0.0;
    for (std::size_t i = 0; i < size_; ++i) mode += to_mode_[k * size_ + i] * n[i];

    // y e^(l dt) + g (e^(l dt) - 1) / l, the last factor through expm1: it
    // is then right for every l <= 0, tending to dt as l dt goes to zero
    // and to -1 / l as it goes to minus infinity, with nothing to cancel.
    const double rate = rate_[k];
    const double exponent = rate * dt;
    if (rate != 0.0) mode *= std::exp(exponent);
    if (drive_[k] != 0.0) mode += drive_[k] * (exponent != 0.0 ? std::expm1(exponent) / rate : dt);
    y[k] = mode;
  }

  for (std::size_t i = 0; i < size_; ++i) {
    double value = 0.0;
    for (std::size_t k = 0; k < size_; ++k) value += to_amount_[i * size_ + k] * y[k];
    // The exact solution cannot go below zero; rounding can, by a little.
    n[i] = value > 0.0 ? value : 0.0;
  }
}

}  // namespace pychron::sim
