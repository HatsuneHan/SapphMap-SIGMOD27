#pragma once

#include <utility>

#include "external_memory/extemvector.hpp"

namespace EM::Sapphmap {

/// MergeSorted with heterogeneous input types, outputs OutVecType.
/// v1 and v2 can be different vector types (e.g. BndCachedVec and StdVector).
template <template <class, auto...> class OutVecType,
          typename InVec1, typename InVec2>
std::pair<OutVecType<uint64_t>, OutVecType<uint8_t>> MergeSortedExtVectorIn(
    InVec1& v1, InVec2& v2,
    const uint64_t* tailValue = nullptr) {
  const uint64_t n1 = v1.size();
  const uint64_t n2 = v2.size();
  const uint64_t outSize = n1 + n2 + (tailValue != nullptr ? 1 : 0);
  std::pair<OutVecType<uint64_t>, OutVecType<uint8_t>> mergeResult{
      OutVecType<uint64_t>(outSize), OutVecType<uint8_t>(outSize)};
  typename OutVecType<uint64_t>::Writer writerVal(mergeResult.first.begin(),
                                                  mergeResult.first.end());
  typename OutVecType<uint8_t>::Writer writerSrc(mergeResult.second.begin(),
                                                mergeResult.second.end());
  typename InVec1::Reader r1(v1.begin(), v1.end());
  typename InVec2::Reader r2(v2.begin(), v2.end());

  uint64_t idx1 = 0, idx2 = 0;
  bool v1Valid = (n1 > 0), v2Valid = (n2 > 0);
  uint64_t curr1 = 0, curr2 = 0;
  if (v1Valid) {
    curr1 = r1.read();
    ++idx1;
  }
  if (v2Valid) {
    curr2 = r2.read();
    ++idx2;
  }
  auto proceed1 = [&]() {
    if (idx1 < n1) {
      curr1 = r1.read();
      ++idx1;
    } else {
      v1Valid = false;
    }
  };
  auto proceed2 = [&]() {
    if (idx2 < n2) {
      curr2 = r2.read();
      ++idx2;
    } else {
      v2Valid = false;
    }
  };
  while (v1Valid || v2Valid) {
    const bool take1 = v1Valid && (!v2Valid || (curr1 <= curr2));
    if (take1) {
      writerVal.write(curr1);
      writerSrc.write(0);
      proceed1();
    } else {
      writerVal.write(curr2);
      writerSrc.write(1);
      proceed2();
    }
  }
  if (tailValue != nullptr) {
    writerVal.write(*tailValue);
    writerSrc.write(2);
  }
  writerVal.flush();
  writerSrc.flush();
  return mergeResult;
}

}  // namespace EM::Sapphmap
