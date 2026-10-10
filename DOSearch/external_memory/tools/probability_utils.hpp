#pragma once

#include <algorithm>
#include <cmath>
#include <utility>

#include "common/utils.hpp"

namespace EM::Sapphmap {

INLINE double alphaLaplace(uint64_t k, double lambda, double beta) {
  Assert(lambda > 0.0);
  Assert(beta > 0.0 && beta < 1.0);

  const double log_term = obliLog(2.0 / beta);
  const double bound1 =
      2.0 * std::sqrt(2.0 * static_cast<double>(k) * log_term);
  const double bound2 = 4.0 * log_term;

  double max_bound = bound1;
  obliMove(bound2 > bound1, max_bound, bound2);
  return lambda * max_bound;
}

INLINE double alphaBM(uint64_t T, double eps, double delta) {
  Assert(T > 0);
  Assert(eps > 0.0);
  Assert(delta > 0.0 && delta < 1.0);

  const uint64_t k = CeilLog2(T);
  const uint64_t floor_log = FloorLog2(T);
  const double lambda = (static_cast<double>(floor_log) + 1.0) / eps;
  const double beta = delta / static_cast<double>(T);

  return alphaLaplace(k, lambda, beta);
}

INLINE double SampleLaplace(double scale) {
  const double u = UniformRandomDoubleExclusive();
  double r = 2.0 * u - 1.0;
  double magnitude = -scale * obliLog(1.0 - std::abs(r));
  return std::copysign(magnitude, r);
}

INLINE double SampleTruncLaplace(double scale, double t) {
  const double u = UniformRandomDoubleExclusive();
  const double probability_cap = 1.0 - obliExp(-t / scale);
  double r = (2.0 * u - 1.0) * probability_cap;
  double magnitude = -scale * obliLog(1.0 - std::abs(r));
  return std::copysign(magnitude, r);
}

INLINE double TruncLaplaceDiffTailProb(double s, double scale, double t) {
  const double inv_b = 1.0 / scale;
  const double lambda_t = t * inv_b;
  const double two_t = 2.0 * t;
  
  const bool s_neg = (s < 0.0);
  double s_abs = s;
  obliMove(s_neg, s_abs, -s);
  double s_clip = s_abs;
  obliMove(s_abs >= two_t, s_clip, two_t);

  const double exp_neg_t = obliExp(-lambda_t);
  const double exp_neg_2t = exp_neg_t * exp_neg_t;
  const double lambda_s = s_clip * inv_b;
  const double exp_neg_s = obliExp(-lambda_s);

  // normalization constant
  const double Z = 1.0 - exp_neg_t;
  const double Z_sq = Z * Z;

  // Case A: s >= t
  const double termA = (2.0 * t - s_clip) * inv_b - 1.0;
  const double numeratorA = 0.25 * exp_neg_s * termA + 0.25 * exp_neg_2t;

  // Case B: s < t
  const double term1 = exp_neg_s * (0.5 + 0.25 * lambda_s);
  const double term2 = -exp_neg_t;
  const double term3 = 0.25 * exp_neg_2t;
  const double term4 =
      0.25 * obliExp(lambda_s - 2.0 * lambda_t);  // Avoid Inf * 0.
  const double numeratorB = term1 + term2 + term3 + term4;

  double numerator = numeratorB;
  obliMove(s_clip >= t, numerator, numeratorA);

  double p = numerator / Z_sq;

  // 3. by symmetry for s < 0
  const double p_sym = 1.0 - p;
  obliMove(s_neg, p, p_sym);

  // 4. handle boundary cases outside the range
  obliMove(s >= two_t, p, 0.0);
  obliMove(s <= -two_t, p, 1.0);

  // 5. clamp to [0, 1] for potential floating point error
  obliMove(p < 0.0, p, 0.0);
  obliMove(p > 1.0, p, 1.0);

  return p;
}
// ---------------------------------------------------------------------------
// Composition Theorems — Public-Parameter Utilities
// ---------------------------------------------------------------------------
// eps_total, delta_total, and k are public parameters, so all control flow
// and math here uses standard (non-oblivious) C++ operations.

/// Bisection root-finding: finds x in [lo, hi] such that f(x) ≈ 0.
/// Requires a sign change: f(lo) and f(hi) must have opposite signs.
/// Returns the midpoint of the final bracket.
template <typename F>
double bisect(F f, double lo, double hi, double tol = 1e-9,
              int max_iter = 100) {
  double f_lo = f(lo);
  double f_hi = f(hi);
  // No sign change — return midpoint as a safe fallback.
  if (f_lo * f_hi > 0.0) return (lo + hi) / 2.0;
  for (int i = 0; i < max_iter; ++i) {
    const double mid = (lo + hi) / 2.0;
    if ((hi - lo) < tol) return mid;
    const double f_mid = f(mid);
    if (f_mid == 0.0) return mid;
    if (f_lo * f_mid < 0.0) {
      hi = mid;
      f_hi = f_mid;
    } else {
      lo = mid;
      f_lo = f_mid;
    }
  }
  return (lo + hi) / 2.0;
}

/// Basic (naïve) composition: uniformly splits eps and delta across k rounds.
INLINE std::pair<double, double> basic_composition(double eps_total,
                                                   double delta_total,
                                                   uint64_t k) {
  const double dk = static_cast<double>(k);
  return {eps_total / dk, delta_total / dk};
}

/// Optimal composition following Kairouz, Oh & Viswanath 2015, Theorem 3.4.
/// Returns (eps_per_round, delta_per_round) that, when composed k times, meet
/// the overall (eps_total, delta_total) guarantee.
INLINE std::pair<double, double> advanced_composition_Kairouz(
    double eps_total, double delta_total, uint64_t k) {
  const double dk = static_cast<double>(k);
  const double tilde_delta = delta_total / 2.0;

  // --- per-round delta via (1-delta_total) = (1-delta_adv)^k (1-tilde_delta)
  double val = (1.0 - delta_total) / (1.0 - tilde_delta);
  if (val < 0.0) val = 0.0;
  const double delta_adv = 1.0 - std::pow(val, 1.0 / dk);

  // --- solve for eps_adv by root-finding on the composition bound
  const double euler_e = std::exp(1.0);

  auto f = [&](double eps) -> double {
    if (eps <= 0.0) return -eps_total;

    const double exp_eps = std::exp(eps);
    // Bound 1: trivial linear composition
    const double term1 = dk * eps;
    // Common drift term: k · eps · (e^eps - 1) / (e^eps + 1)
    const double drift =
        (exp_eps - 1.0) * eps * dk / (exp_eps + 1.0);
    // Bound 2: advanced composition (data-dependent log argument)
    const double term2 =
        drift +
        eps * std::sqrt(2.0 * dk *
                        std::log(euler_e + std::sqrt(dk) * eps /
                                               tilde_delta));
    // Bound 3: advanced composition (simpler log argument)
    const double term3 =
        drift + eps * std::sqrt(2.0 * dk * std::log(1.0 / tilde_delta));

    return std::min(term1, std::min(term2, term3)) - eps_total;
  };

  // f(0) = -eps_total < 0 and f(eps_total) >= 0 when k >= 1.
  const double f_lo = f(0.0);
  const double f_hi = f(eps_total);
  double eps_adv;
  if (f_lo * f_hi > 0.0) {
    // Fallback to basic composition (should not happen in practice).
    eps_adv = eps_total / dk;
  } else {
    eps_adv = bisect(f, 0.0, eps_total);
  }

  return {eps_adv, delta_adv};
}

}  // namespace EM::Sapphmap
