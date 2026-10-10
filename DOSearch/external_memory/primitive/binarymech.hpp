#pragma once

#include <cmath>
#include <limits>
#include <type_traits>
#include <iterator>
#include <utility>
#include <memory>

#include "common/defs.hpp"
#include "common/utils.hpp"
#include "external_memory/tools/probability_utils.hpp"

namespace EM::Primitive {

INLINE uint64_t ClampNoisyCount(double val) {
  const double maxVal = static_cast<double>(std::numeric_limits<uint64_t>::max());
  const uint64_t maxOut = std::numeric_limits<uint64_t>::max();

  uint64_t res = static_cast<uint64_t>(obliRound(val));

  obliMove(val <= 0.0, res, static_cast<uint64_t>(0));
  obliMove(val >= maxVal, res, maxOut);
  return res;
}

/// @brief DP prefix sum using binary mechanism (streaming, sequential I/O)
/// @tparam OutputVector output vector type (e.g., ExtVector::Vector<uint64_t>)
/// @tparam Iterator input iterator
/// @tparam GetValue functor to extract uint64_t value from input element
/// @param begin input begin
/// @param end input end
/// @param getValue functor returning uint64_t-like value
/// @param dpPrefix output DP prefix sum (pre-sized to input length)
/// @param eps privacy budget (epsilon)
/// @param delta privacy parameter (delta)
/// @param trueSum output total sum (last true prefix)
/// @return true if all DP prefixes within [true-eta, true+eta]
template <class OutputVector, class Iterator, class GetValue>
bool BinaryMechPrefixSum(Iterator begin, Iterator end, const GetValue& getValue,
                         OutputVector& dpPrefix,
                         double eps, double delta, uint64_t& trueSum) {
  using InputValueT = typename std::iterator_traits<Iterator>::value_type;
  using GetValueResultT =
      decltype(getValue(std::declval<const InputValueT&>()));
  static_assert(std::is_convertible<GetValueResultT, uint64_t>::value,
                "BinaryMechPrefixSum getValue must return uint64_t-convertible");
  using OutputValueT = typename OutputVector::Iterator::value_type;
  static_assert(std::is_same<OutputValueT, uint64_t>::value,
                "BinaryMechPrefixSum requires uint64_t output vectors");

  const uint64_t n = static_cast<uint64_t>(end - begin);
  trueSum = 0;

  double eta = EM::Sapphmap::alphaBM(n, eps, delta);

  typename OutputVector::Writer dpWriter(dpPrefix.begin(),
                                         dpPrefix.begin() + n);

  const uint64_t maxLevel = obliMax<uint64_t>(1, FloorLog2(n) + 1);
  const double noise_scale =
      static_cast<double>(maxLevel) / eps;
      
  std::unique_ptr<uint64_t[]> trueStack(new uint64_t[maxLevel]());
  std::unique_ptr<double[]> noisyStack(new double[maxLevel]());
  std::unique_ptr<uint8_t[]> active(new uint8_t[maxLevel]());
  double activeSum = 0.0;

  using InputVector = typename Iterator::vector_type;
  typename InputVector::Reader inputReader(begin, end);

  uint8_t utility = 1;
  uint64_t prevDpVal = 0;
  for (uint64_t i = 0; i < n; ++i) {
    const auto& inputVal = inputReader.read();
    uint64_t trueVal = static_cast<uint64_t>(getValue(inputVal));
    trueSum += trueVal;

    const uint64_t carry = __builtin_ctzll(
        ~static_cast<unsigned long long>(i));
    uint64_t level = 0;

    for (uint64_t k = 0; k < carry; ++k) {
      activeSum -= noisyStack[level];
      trueVal += trueStack[level];
      active[level] = 0;
      ++level;
    }

    double noisyVal = static_cast<double>(trueVal) +
                      EM::Sapphmap::SampleLaplace(noise_scale);

    activeSum += noisyVal;
    trueStack[level] = trueVal;
    noisyStack[level] = noisyVal;
    active[level] = 1;

    uint64_t dpVal = ClampNoisyCount(activeSum);
    // Post-processing: prefix sum must be monotonically non-decreasing.
    // Does not affect DP (post-processing theorem).
    dpVal = obliMax(dpVal, prevDpVal);
    prevDpVal = dpVal;
    dpWriter.write(dpVal);

    // check utility
    const double lower = static_cast<double>(trueSum) - eta;
    const double upper = static_cast<double>(trueSum) + eta;
    const double dpValD = static_cast<double>(dpVal);
    const uint8_t fail =
        static_cast<uint8_t>((dpValD < lower) | (dpValD > upper));
    utility = static_cast<uint8_t>(utility & static_cast<uint8_t>(!fail));
  }

  dpWriter.flush();

  return utility == 1;
}

}  // namespace EM::Primitive
