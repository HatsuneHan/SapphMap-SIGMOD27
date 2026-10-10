#pragma once

#include "common/defs.hpp"
#include "common/mov_intrinsics.hpp"
#include "common/utils.hpp"
#include "external_memory/primitive/sorting/sort_def.hpp"

namespace EM::Primitive {
/// @brief Deterministic oblivious distribute (Algorithm 3).
/// @tparam ValueType element type
/// @param X input array (size n), expanded to size m
/// @param f destination mapping array (size n, 1-based indices)
/// @param m target size
template <class ValueType>
void FODistribute(std::vector<ValueType>& X, std::vector<uint32_t>& f,
                  uint32_t m) {
  const uint32_t n = static_cast<uint32_t>(X.size());

  const ValueType dummy = ValueType();
  for (uint32_t i = n; i < m; ++i) {
    X.push_back(dummy);
    f.push_back(UINT32_MAX);
  }

  uint32_t j = 1UL << (CeilLog2(m) - 1);
  while (j >= 1) {
    for (uint32_t i = m - j; i-- > 0;) {
      const bool swapFlag = (f[i] >= (i + j)) & (f[i] != UINT32_MAX);
      condSwap(swapFlag, X[i], X[i + j]);
      condSwap(swapFlag, f[i], f[i + j]);
    }
    j >>= 1;
  }
}

}  // namespace EM::Primitive
