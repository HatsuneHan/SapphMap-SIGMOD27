#pragma once

#include <algorithm>
#include <cmath>
#include <iterator>
#include <type_traits>
#include <vector>
#include <memory>
#include "external_memory/dynamicvector.hpp"
#include "external_memory/noncachedvector.hpp"
#include "external_memory/extemvector.hpp"
#include "external_memory/primitive/binarymech.hpp"
#include "external_memory/primitive/compaction/or_compact_shuffle.hpp"


namespace EM::Primitive {

/// @brief Differentially oblivious compaction (streaming, sequential I/O)
/// @tparam Iterator input/output iterator
/// @tparam Check predicate for marked elements (1-balls)
/// @param begin input begin
/// @param end input end
/// @param isMarked predicate for marked elements
/// @param eps privacy budget (epsilon)
/// @param delta privacy parameter (delta)
template <class Iterator, class Check>
void DOCompact(Iterator begin, Iterator end, const Check& isMarked, double eps,
               double delta, uint32_t inAuth = 0, uint64_t earlyStop = 0) {
  using ValueType = typename std::iterator_traits<Iterator>::value_type;
  const uint64_t n = end - begin;
  if (n == 0) {
    return;
  }

  // Step 1: compute eta using alphaBM (fixed for entire stream).
  uint64_t eta = 
    obliFloorU64(EM::Sapphmap::alphaBM(n, eps, delta));
  eta = obliMax<uint64_t>(eta, 1);

  std::vector<ValueType> workingBuffer;
  workingBuffer.reserve(3 * eta);
  size_t head = 0;

  using InputVector = typename Iterator::vector_type;
  using InputReader = typename InputVector::Reader;
  InputReader inputReader = [&]() -> InputReader {
    if constexpr (std::is_constructible_v<InputReader, Iterator, Iterator,
                                          uint32_t>) {
      return InputReader(begin, end, inAuth);
    } else {
      return InputReader(begin, end);
    }
  }();
  using OutputVector = typename Iterator::vector_type;
  using OutputWriter = typename OutputVector::Writer;
  OutputWriter outputWriter = [&]() -> OutputWriter {
    if constexpr (std::is_constructible_v<OutputWriter, Iterator, Iterator,
                                          uint32_t>) {
      return OutputWriter(begin, end, inAuth + 1);
    } else {
      return OutputWriter(begin, end);
    }
  }();

  // Binary mechanism state (streaming, sequential I/O).
  const uint64_t maxLevel = obliMax<uint64_t>(1, FloorLog2(n) + 1);
  const double noise_scale = static_cast<double>(maxLevel) / eps;
  std::unique_ptr<uint64_t[]> trueStack(new uint64_t[maxLevel]());
  std::unique_ptr<double[]> noisyStack(new double[maxLevel]());
  std::unique_ptr<uint8_t[]> active(new uint8_t[maxLevel]());
  
  double activeSum = 0.0;

  uint64_t processed = 0;
  uint64_t outCount = 0;
  uint64_t trueSum = 0;
  uint64_t dpVal = 0;
  uint64_t prevDpVal = 0;
  // uint8_t utility = 1;

  while (processed < n) {
    const uint64_t remaining = n - processed;
    const uint64_t toFetch = obliMin<uint64_t>(eta, remaining);

    for (uint64_t i = 0; i < toFetch; ++i) {
      const auto& inputVal = inputReader.read();
      workingBuffer.push_back(inputVal);

      uint64_t trueVal = static_cast<uint64_t>(isMarked(inputVal));
      trueSum += trueVal;
      
      const uint64_t carry = __builtin_ctzll(
          ~static_cast<unsigned long long>(processed + i));

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
    }

    dpVal = ClampNoisyCount(activeSum);
    // Post-processing: prefix sum must be monotonically non-decreasing.
    dpVal = obliMax(dpVal, prevDpVal);
    prevDpVal = dpVal;
    processed += toFetch;

    {
      // check utility using clamped dp value
      const double lower = static_cast<double>(trueSum) - eta;
      const double upper = static_cast<double>(trueSum) + eta;
      const double dpValD = static_cast<double>(dpVal);
      const uint8_t fail =
          static_cast<uint8_t>((dpValD < lower) | (dpValD > upper));
      // utility = static_cast<uint8_t>(utility & static_cast<uint8_t>(!fail));

      // degradation, fallback to non-oblivious compaction
      if (fail) {
        printf("DOCompact fallback to non-oblivious compaction\n");
        for (const auto& val : workingBuffer) {
          if (isMarked(val)) {
            outputWriter.write(val);
            ++outCount;
          }
        }

        for (uint64_t i = processed; i < n; ++i) {
          const auto& inputVal = inputReader.read();
          if (isMarked(inputVal)) {
            outputWriter.write(inputVal);
            ++outCount;
          }
        }

        const ValueType dummy = ValueType();
        while (outCount < n) {
          outputWriter.write(dummy);
          ++outCount;
        }
        outputWriter.flush();
        return;
      }
    }
    
    // ORCompact the active working buffer.
    OrCompact(workingBuffer.begin(), workingBuffer.end(), isMarked);

    // Determine target output size using current DP prefix sum.
    const uint64_t diff = dpVal - eta;
    uint64_t target = 0;
    obliMove(dpVal >= eta, target, diff);
    obliMove(target > n, target, n);

    while (outCount < target) {
      outputWriter.write(workingBuffer[head]);
      ++outCount;
      ++head;
    }

    workingBuffer.erase(workingBuffer.begin(), workingBuffer.begin() + head);
    head = 0;

    // safe branch: working buffer size is public
    if (workingBuffer.size() > 2 * eta) {
      workingBuffer.resize(2 * eta);
    }

  }

  const uint64_t maxOutCount = earlyStop > 0 ? earlyStop : n;
  const uint64_t toFlush =
      obliMin<uint64_t>(maxOutCount - outCount, workingBuffer.size());

  for (uint64_t i = 0; i < toFlush; ++i) {
    outputWriter.write(workingBuffer[head]);
    ++outCount;
    ++head;
  }

  const ValueType dummy = ValueType();
  while (outCount < maxOutCount) {
    outputWriter.write(dummy);
    ++outCount;
  }

  outputWriter.flush();
}

}  // namespace EM::Primitive
