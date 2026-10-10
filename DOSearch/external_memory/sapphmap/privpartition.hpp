#pragma once

/**
 * @file privpartition.hpp
 * @brief Privacy-preserving partition and merge kernels for SapphMap.
 *
 * This header implements the core DP partition pipeline used by PrivDS:
 * partition boundary sampling (DOPartition), item assignment (DOItemAssign),
 * and heterogeneous run merge (DOPMerge).
 */

#include <cassert>
#include <cmath>
#include <iterator>
#include <memory>
#include <new>
#include <type_traits>
#include "common/defs.hpp"
#include "external_memory/tools/probability_utils.hpp"
#include "common/utils.hpp"
#include "external_memory/primitive/binarymech.hpp"
#include "external_memory/primitive/compaction/do_compact.hpp"
#include "external_memory/primitive/sorting/bitonic.hpp"
#include "external_memory/primitive/sorting/kway_butterfly_sort.hpp"
#include "external_memory/extemvector.hpp"
#include "external_memory/tools/mergesorted.hpp"
#include "external_memory/sapphmap/pds_def.hpp"
#include "external_memory/primitive/distribution/fo_distribute.hpp"
#include "external_memory/stdvector.hpp"
#include <vector>
#include <utility>

namespace EM::Sapphmap {

/**
 * @brief One sampled boundary record before oblivious compaction.
 */

struct DOPartitionEntry {
  uint64_t boundary_key = 0;
  uint64_t preceding_item_count = 0;
  uint8_t is_marked = 0;

  DOPartitionEntry() = default;
  DOPartitionEntry(uint64_t bkey, uint64_t count, uint8_t marked)
      : boundary_key(bkey), preceding_item_count(count), is_marked(marked) {}
};

template <template <class, auto...> class VectorType,
          template <class, auto...> class CachedVectorType = EM::ExtVector::Vector>
struct DOPartitionResult {
  /// Padded segment capacity aligned to transfer block size.
  uint64_t Z = 0;
  /// Raw (unpadded) segment capacity before alignment.
  uint64_t Z_raw = 0;
  std::unique_ptr<CachedVectorType<uint64_t>> boundaries;
  std::unique_ptr<VectorType<uint64_t>> counts;
};

template <template <class, auto...> class OutVecType,
          template <class, auto...> class CachedVecType>
struct DOPMergeResult {
  /// Padded segment capacity aligned to transfer block size.
  uint64_t Z = 0;
  /// Raw (unpadded) segment capacity before alignment.
  uint64_t Z_raw = 0;
  std::unique_ptr<OutVecType<EM::Definition::Item>> items;
  std::unique_ptr<CachedVecType<uint64_t>> boundaries;
  std::unique_ptr<OutVecType<uint64_t>> counts;
};

/// DO private partition for sorted distinct keys.
/// Samples DP-noisy partition boundaries using truncated Laplace / geometric
/// mechanism, then compacts marked entries via FO OrCompact.
template <template <class, auto...> class VectorType,
          template <class, auto...> class CachedVectorType = EM::ExtVector::Vector,
          template <class, auto...> class IntmdVecType = VectorType,
          class Iterator,
          class GetKey>
DOPartitionResult<VectorType, CachedVectorType>
DOPartition(Iterator begin, Iterator end, const GetKey& getKey,
            uint64_t& count, bool c_full, 
            double eps_svt, double delta_svt,
            uint64_t cache_page_size = BLOCK_SIZE, uint32_t inAuth = 0) {
  using InputValueT = typename std::iterator_traits<Iterator>::value_type;
  using InputVector = typename Iterator::vector_type;
  const uint64_t n = static_cast<uint64_t>(end - begin);

  // When c_full is set, include one extra terminal step that samples
  // against DOMAIN_SIZE_D as a synthetic tail boundary.
  uint64_t loopCount = n - 1;
  obliMove(c_full, loopCount, n);

  typename CachedVectorType<uint64_t>::Writer boundaryWriter;
  typename VectorType<uint64_t>::Writer countWriter;

  const uint64_t effective_block_size = cache_page_size - 32ULL;
  uint64_t items_per_block = effective_block_size / sizeof(EM::Definition::Item);
  items_per_block = obliMax<uint64_t>(items_per_block, 1ULL);

  const double t =
      (2.0 * obliLog((2.0 * static_cast<double>(EM::Definition::DOMAIN_SIZE_D) *
                      (2.0 * obliExp(eps_svt) + delta_svt)) /
                     delta_svt)) / eps_svt;
  uint64_t Z_raw = obliCeilU64(5.0 * t);
  uint64_t Z_out = ((Z_raw + items_per_block - 1) / items_per_block) * items_per_block;

  // Choose tau from padded capacity so both lower and upper utility
  // constraints remain valid after block-size alignment.

  const double tau = static_cast<double>(Z_out) - 2.0 * t;

  CachedVectorType<DOPartitionEntry> seal(loopCount);
  typename CachedVectorType<DOPartitionEntry>::Writer sealWriter(
      seal.begin(), seal.end());

  typename InputVector::Reader inputReader(begin, end, inAuth);

  // Initialize state from the first observed element.
  InputValueT currItem = inputReader.read();
  uint64_t lastRealKey = static_cast<uint64_t>(getKey(currItem));
  bool lastRealKeyValid = !currItem.is_dummy();

  uint64_t sealMarkedCount = 0;
  
  for (uint64_t i = 0; i < loopCount; ++i) {
    uint64_t nextKey = 0;
    bool nextIsDummy = false;
    
    // The loop index is public, so this branch preserves obliviousness.
    // Read count remains exactly n-1 for real input items.
    if (i == n - 1) {
      nextKey = EM::Definition::DOMAIN_SIZE_D;
      nextIsDummy = false;
    } else {
      InputValueT nextItem = inputReader.read();
      nextKey = static_cast<uint64_t>(getKey(nextItem));
      nextIsDummy = nextItem.is_dummy();
    }

    // Skip effective sampling if next is dummy or no real key has appeared yet.
    const bool skipOutput = nextIsDummy | (!lastRealKeyValid);
    
    // Count advances only when a real sampling step is active.
    obliMove(!skipOutput, count, count + static_cast<uint64_t>(1));

    // Always execute the full oblivious sampling math, including dummy cases.
    const double p =
        EM::Sapphmap::TruncLaplaceDiffTailProb(tau - static_cast<double>(count), 2.0 / eps_svt, t);

    double val = 0.5;
    obliMove((p > 0.0) & (p < 1.0), val, 1.0 - p);

    const double u = UniformRandomDoubleExclusive();
    double log_val = obliLog(val);
    obliMove(val == 1.0, log_val, -p);
    
    uint64_t offset = obliCeilU64(obliLog(u) / log_val) - 1ULL;
    obliMove(p == 1.0, offset, static_cast<uint64_t>(0));
    
    // Clamp offset to prevent uint64 overflow in key reconstruction.
    uint64_t max_offset = std::numeric_limits<uint64_t>::max() - lastRealKey;
    obliMove(offset > max_offset, offset, max_offset);
    uint64_t idx = lastRealKey + offset;

    // Mark only valid boundaries from non-skipped sampling steps.
    const bool flag = !skipOutput & (p > 0.0) & (idx < nextKey);
    sealMarkedCount += static_cast<uint64_t>(flag);

    // Write every sampled record uniformly; invalid candidates are filtered by
    // the mark bit during oblivious compaction.
    sealWriter.write(
        DOPartitionEntry(idx, count, static_cast<uint8_t>(flag)));

    // Reset segment count when a boundary is accepted.
    obliMove(flag, count, static_cast<uint64_t>(0));

    // Update the running real key only for non-dummy, non-terminal elements.
    const bool updateLast = (!nextIsDummy) & (i != n - 1);
    obliMove(updateLast, lastRealKey, nextKey);
    obliMove(updateLast, lastRealKeyValid, true);
  }

  sealWriter.flush();

  // Obliviously compact marked boundaries to the front.
  auto isMarked = [](const DOPartitionEntry& entry) -> bool {
    return entry.is_marked == 1;
  };

  EM::Primitive::OrCompact(seal.begin(), seal.end(), isMarked);

  DOPartitionResult<VectorType, CachedVectorType> result;
  result.Z = Z_out;
  result.Z_raw = Z_raw;
  result.boundaries = std::make_unique<CachedVectorType<uint64_t>>(sealMarkedCount);
  result.counts = std::make_unique<VectorType<uint64_t>>(sealMarkedCount + 1);
  boundaryWriter.init(result.boundaries->begin(), result.boundaries->end());
  countWriter.init(result.counts->begin(), result.counts->end());

  typename CachedVectorType<DOPartitionEntry>::Reader sealReader(
      seal.begin(), seal.begin() + sealMarkedCount);

  for (uint64_t i = 0; i < sealMarkedCount; ++i) {
    const DOPartitionEntry& entry = sealReader.read();
    boundaryWriter.write(entry.boundary_key);
    countWriter.write(entry.preceding_item_count);
  }

  countWriter.write(count);
  boundaryWriter.flush();
  countWriter.flush();

  return result;
}

template <template <class, auto...> class VectorType,
          template <class, auto...> class CachedVectorType = EM::ExtVector::Vector,
          template <class, auto...> class IntmdVecType = VectorType,
          class Iterator,
          class GetKey>
/**
 * @brief Assign items into fixed-size DP segments after DOPartition.
 *
 * The function uses noisy prefix sums and oblivious distribution so segment
 * occupancy and movement patterns are data-oblivious.
 */
IntmdVecType<typename std::iterator_traits<Iterator>::value_type>
DOItemAssign(Iterator begin, Iterator end, const GetKey& getKey,
             CachedVectorType<uint64_t>& boundaries,
             VectorType<uint64_t>& counts, double eps, double delta, uint64_t Z,
             uint32_t inAuth = 0) {
  using InputValueT = typename std::iterator_traits<Iterator>::value_type;
  const uint64_t n = static_cast<uint64_t>(end - begin);
  const uint64_t m = boundaries.size();
  
  uint64_t eta =
      obliFloorU64(EM::Sapphmap::alphaBM(m + 1, eps / 2.0, delta));
  eta = obliMax<uint64_t>(eta, 1);

  uint64_t w =
    obliCeilU64(static_cast<double>(eta) / static_cast<double>(Z));

  VectorType<uint64_t> dpPrefix(m + 1);
  auto getCount = [](const uint64_t& val) { return val;};
  uint64_t trueSum = 0;
  bool utility = EM::Primitive::BinaryMechPrefixSum(
      counts.begin(), counts.end(), getCount, dpPrefix,
      eps / 2.0, delta, trueSum);

  obliMove(!utility, eta, static_cast<uint64_t>(0));
  obliMove(!utility, w, static_cast<uint64_t>(1));

  const uint64_t outputSize = (m+1) * Z;
  IntmdVecType<InputValueT> output(outputSize);
  typename IntmdVecType<InputValueT>::Writer outputWriter(
      output.begin(), output.end());

  using InputVector = typename Iterator::vector_type;
  typename InputVector::Reader inputReader(begin, end, inAuth);
  typename CachedVectorType<uint64_t>::Reader boundaryReader(
      boundaries.begin(), boundaries.end());
  typename VectorType<uint64_t>::Reader dpReader(dpPrefix.begin(),
                                                dpPrefix.end());
  typename VectorType<uint64_t>::Reader countReader(
      counts.begin(), counts.end());

  uint64_t prefixIdx = 0;
  uint64_t prefixVal = 0;
  uint64_t countIdx = 0;
  uint64_t countPrefixVal = 0;
  uint64_t readCount = 0;

  std::vector<uint64_t> boundaryBatch; 
  boundaryBatch.reserve(w);
  std::vector<InputValueT> buffer;
  buffer.reserve(w * Z + 4 * eta);

  auto isMarked = [](const InputValueT& item) { return !item.is_dummy(); };
  auto readPrefixTo = [&](uint64_t targetIdx) {
    if (utility) {
      while (prefixIdx <= targetIdx) {
        prefixVal = dpReader.read();
        ++prefixIdx;
      }
    } else {
      while (countIdx <= targetIdx) {
        countPrefixVal += countReader.read();
        ++countIdx;
      }
      prefixVal = countPrefixVal;
    }
  };

  const uint64_t iterations = obliCeilU64(static_cast<double>(m+1) / static_cast<double>(w));
  for (uint64_t i = 0; i < iterations; ++i) {
    const uint64_t segStart = i * w;
    const uint64_t segEnd =
        obliMin<uint64_t>((i + 1) * w, m + 1) - 1;
    boundaryBatch.clear();
    const uint64_t boundaryEnd = obliMin<uint64_t>(segEnd, m - 1);
    if(m > 0){
      for (uint64_t k = segStart; k <= boundaryEnd; ++k) {
        boundaryBatch.push_back(boundaryReader.read());
      }
    }
    boundaryBatch.push_back(EM::Definition::DOMAIN_SIZE_D);

    readPrefixTo(segEnd);
    const uint64_t targetRead =
        obliMin<uint64_t>(prefixVal + eta, n);

    while (readCount < targetRead) {
      buffer.push_back(inputReader.read());
      ++readCount;
    }

    EM::Primitive::OrCompact(buffer.begin(), buffer.end(), isMarked);

    const uint64_t bufferSize = static_cast<uint64_t>(buffer.size());
    std::vector<uint32_t> mapping(bufferSize, 0);

    uint64_t k = segStart;
    uint64_t d = 0;

    for (uint64_t j = 0; j < bufferSize - 1; ++j) {
      mapping[j] = (k - segStart) * Z + d;

      const uint64_t currBoundary = boundaryBatch[k - segStart];
      const bool c_nxt = (k <= segEnd) 
        & (!buffer[j].is_dummy()) & (!buffer[j+1].is_dummy())
        & (getKey(buffer[j]) <= currBoundary) 
        & (currBoundary < getKey(buffer[j + 1]));

      obliMove(c_nxt, k, k + 1);
      obliMove(c_nxt, ++d, static_cast<uint64_t>(0));
    }
    mapping[bufferSize - 1] = (k - segStart) * Z + d;

    EM::Primitive::FODistribute(buffer, mapping, static_cast<uint32_t>(w*Z+4*eta));

    for (uint64_t seg = segStart; seg <= segEnd; ++seg) {
      const uint64_t segOffset = (seg - segStart) * Z;
      for (uint64_t j = 0; j < Z; ++j) {
        outputWriter.write(buffer[segOffset + j]);
      }
    }

    const uint64_t writeCount = (segEnd - segStart + 1) * Z;
    buffer.erase(buffer.begin(), buffer.begin() + writeCount);

    if (buffer.size() > 2 * eta) {
      buffer.resize(2 * eta);
    }
  }
  
  outputWriter.flush();
  
  return output;
}

/// Merge two heterogeneous partition-based PDSs (DOPMerge).
template <template <class, auto...> class OutVecType,
          template <class, auto...> class CachedVecType,
          template <class, auto...> class IntmdVecType = OutVecType,
          typename BndVec1, typename BndVec2,
          typename ItemVec1, typename ItemVec2>
/**
 * @brief Merge two partitioned runs and rebuild boundaries/counts/items.
 *
 * The merge is written to support heterogeneous memory back-ends so enclave
 * and external runs can be combined without changing merge semantics.
 */
DOPMergeResult<OutVecType, CachedVecType> DOPMerge(BndVec1& boundaries1,
                                   BndVec2& boundaries2,
                                   ItemVec1& items1,
                                   ItemVec2& items2,
                                   double eps_svt, double delta_svt,
                                   double eps_op, double delta_op, uint64_t Z,
                                   uint64_t cache_page_size,
                                   uint64_t total_real_count,
                                   bool remove_deletions, uint32_t inAuth = 0) {
  using Item = EM::Definition::Item;
  using OpType = EM::Definition::OpType;
  const uint64_t p = boundaries1.size();
  const uint64_t q = boundaries2.size();

  const uint64_t domainTail = EM::Definition::DOMAIN_SIZE_D;
  auto mergedResult =
      MergeSortedExtVectorIn<OutVecType>(boundaries1, boundaries2, &domainTail);
  OutVecType<uint64_t>& mergedBoundaries = mergedResult.first;
  OutVecType<uint8_t>& mergedBoundarySource = mergedResult.second;
  const uint64_t mergedCount = mergedBoundaries.size();

  // Shared working buffer for one merge-and-repartition window.
  StdVector<Item> buffer(3 * Z + 2);

  IntmdVecType<Item> newitemsBuffer(mergedCount * 2 * Z);
  typename IntmdVecType<Item>::Writer newitemsWriter(newitemsBuffer.begin(), newitemsBuffer.end());

  uint64_t maxBoundariesCount = (p + q + 2) * 5;
  IntmdVecType<uint64_t> newboundariesBuffer(maxBoundariesCount);
  typename IntmdVecType<uint64_t>::Writer newboundariesWriter(newboundariesBuffer.begin(), newboundariesBuffer.end());
  IntmdVecType<uint64_t> newcountsBuffer(maxBoundariesCount + 1);
  typename IntmdVecType<uint64_t>::Writer newcountsWriter(newcountsBuffer.begin(), newcountsBuffer.end());

  uint64_t newboundariesWritten = 0;
  uint64_t newcountsWritten = 0;

  typename ItemVec1::Reader items1Reader(items1.begin(), items1.end(), inAuth);
  typename ItemVec2::Reader items2Reader(items2.begin(), items2.end(), inAuth);
  const Item dummyItem = Item();

  auto appendSegment = [&](auto& reader, uint64_t offset) {
    for (uint64_t t = 0; t < Z; ++t) {
      buffer[offset + t] = reader.read();
    }
  };

  auto appendDummies = [&](uint64_t count, uint64_t offset) {
    for (uint64_t t = 0; t < count; ++t) {
      buffer[offset + t] = dummyItem;
    }
  };

  typename OutVecType<uint64_t>::Reader mergedBoundariesReader(
      mergedBoundaries.begin(), mergedBoundaries.begin() + mergedCount);
  typename OutVecType<uint8_t>::Reader mergedBoundarySourceReader(
      mergedBoundarySource.begin(), mergedBoundarySource.begin() + mergedCount);

  uint64_t s_next = mergedBoundariesReader.read();
  uint8_t src_next = mergedBoundarySourceReader.read();

  // Initialize the first window with one source segment, a dummy gap, and one
  // segment from the other source.
  if(src_next == 0){
    appendSegment(items1Reader, 0);
    appendDummies(Z + 2, Z);
    appendSegment(items2Reader, 2 * Z + 2);
  } else {
    appendSegment(items2Reader, 0);
    appendDummies(Z + 2, Z);
    appendSegment(items1Reader, 2 * Z + 2);
  }

  uint64_t running_count = 0;
  uint64_t Z_new = 0;
  uint64_t Z_raw_new = 0;

  for (uint64_t k = 0; k < mergedCount; ++k) {
    const uint64_t s_k = s_next;
    const uint8_t src = src_next;

    if(k < mergedCount - 1){
      s_next = mergedBoundariesReader.read();
      src_next = mergedBoundarySourceReader.read();
    }

    uint64_t segItemCount = 0;
    for (uint64_t idx = 0; idx < Z + 1; ++idx) {
      const bool c_seg = (!buffer[idx].is_dummy()) & (buffer[idx].get_key() <= s_k);
      obliMove(c_seg, segItemCount, segItemCount + 1);
    }
    for (uint64_t idx = 2 * Z + 2; idx < 3 * Z + 2; ++idx) {
      const bool c_seg = (!buffer[idx].is_dummy()) & (buffer[idx].get_key() <= s_k);
      obliMove(c_seg, segItemCount, segItemCount + 1);
    }

    const Item fillerItem = Item(OpType::DMY, s_k, UINT64_MAX, 0, true);
    for (uint64_t idx = 0; idx < 2 * Z + 2; ++idx) {
      const bool c_filler = buffer[idx].is_dummy() & (segItemCount < 2 * Z + 2);
      obliMove(c_filler, buffer[idx], fillerItem);
      obliMove(c_filler, segItemCount, segItemCount + 1);
    }

    // Merge left and right sorted halves into one sorted window.
    EM::Primitive::BitonicMergeTwoSorted(buffer.begin(), buffer.begin() + 2 * Z + 2, buffer.end());

    uint64_t lastKey = 0;
    bool lastKeyValid = false;
    Item x_last = dummyItem;

    // Position 2Z+1 is reserved as carry-over sentinel for the next iteration.
    for (uint64_t idx = 0; idx < 2 * Z + 1; ++idx) {
      const uint64_t key = buffer[idx].get_key();
      const bool is_dup = lastKeyValid & (key == lastKey);
      const bool is_del = remove_deletions & (buffer[idx].get_op() == OpType::DEL) & (key <= s_k);
      const bool is_filler = buffer[idx].is_dummy() & (key == s_k);

      obliMove(is_dup | is_del | is_filler, buffer[idx], dummyItem);
      obliMove(!is_dup, lastKey, key);
      obliMove(!is_dup, lastKeyValid, true);
      
      const bool is_real = !buffer[idx].is_dummy();
      obliMove(is_real, x_last, buffer[idx]);
    }

    // Persist carry-over item at the reserved boundary slot.
    buffer[2 * Z + 1] = x_last;

    auto getKey = [](const Item& item) { return item.get_key(); };
    auto partitioned = DOPartition<StdVector, StdVector, StdVector>(
        buffer.begin(), buffer.begin() + 2 * Z + 1, getKey,
        running_count, k == mergedCount - 1, eps_svt, delta_svt, cache_page_size);

    for (uint64_t idx = 0; idx < partitioned.boundaries->size(); ++idx) {
      newboundariesWriter.write((*partitioned.boundaries)[idx]);
      newcountsWriter.write((*partitioned.counts)[idx]);
      ++newboundariesWritten;
      ++newcountsWritten;
    }
    running_count = (*partitioned.counts)[partitioned.counts->size() - 1];
    Z_new = partitioned.Z;
    Z_raw_new = partitioned.Z_raw;

    const auto isMarkedForCompact = [](const Item& item) { return !item.is_dummy(); };
    EM::Primitive::OrCompact(buffer.begin(), buffer.begin() + 2 * Z + 1, isMarkedForCompact);

    if (k == 0) {
      for (uint64_t idx = 0; idx < 2 * Z; ++idx) newitemsWriter.write(buffer[idx]);
    } else {
      for (uint64_t idx = 1; idx < 2 * Z + 1; ++idx) newitemsWriter.write(buffer[idx]);
    }

    if(k < mergedCount - 1){
      if(src == src_next){
        // Keep source order; rotate carry-over to the first position.
        buffer[0] = buffer[2 * Z + 1];
        // Fill [1, Z] with the next segment from the active source.
        if (src == 0)
          appendSegment(items1Reader, 1);
        else
          appendSegment(items2Reader, 1);
        // Fill middle region with dummies.
        appendDummies(Z + 1, Z + 1);
      } else {
        // Source switched: shift carry-over window to front.
        std::move(buffer.begin() + 2 * Z + 1, buffer.end(), buffer.begin());
        // Rebuild dummy middle gap.
        appendDummies(Z + 1, Z + 1);
        // Load next segment from the new active source.
        if (src == 0)
          appendSegment(items1Reader, 2 * Z + 2);
        else
          appendSegment(items2Reader, 2 * Z + 2);
      }
    }
  }

  newcountsWriter.write(running_count);
  ++newcountsWritten;

  // Pad remaining slots to keep writer/auth state consistent before flush.
  for(uint64_t idx = newboundariesWritten; idx < maxBoundariesCount; ++idx) {
    newboundariesWriter.write(UINT64_MAX);
  }
  for(uint64_t idx = newcountsWritten; idx < maxBoundariesCount + 1; ++idx) {
    newcountsWriter.write(UINT64_MAX);
  }

  newboundariesWriter.flush();
  newcountsWriter.flush();
  newitemsWriter.flush();

  buffer.clear();

  uint64_t maxItemsCount = (p+q+2) * Z;
  obliMove(total_real_count < maxItemsCount, maxItemsCount, total_real_count);

  const auto isMarked = [](const Item& item) { return !item.is_dummy(); };
  EM::Primitive::DOCompact(newitemsBuffer.begin(), newitemsBuffer.end(), isMarked,
                           eps_op / 2.0, delta_op, 0);

  DOPMergeResult<OutVecType, CachedVecType> output;
  output.Z = Z_new;
  output.Z_raw = Z_raw_new;

  auto mergeditems = std::make_unique<OutVecType<Item>>(maxItemsCount);
  {
    typename IntmdVecType<Item>::Reader itemsReader(newitemsBuffer.begin(), newitemsBuffer.end(), 1);
    typename OutVecType<Item>::Writer mergeditemsWriter(mergeditems->begin(), mergeditems->end());
    for (uint64_t idx = 0; idx < maxItemsCount; ++idx) {
      mergeditemsWriter.write(itemsReader.read());
    }
    mergeditemsWriter.flush();
  }
  output.items = std::move(mergeditems);

  auto mergedBoundariesOut = std::make_unique<CachedVecType<uint64_t>>(newboundariesWritten);
  {
    typename IntmdVecType<uint64_t>::Reader bReader(newboundariesBuffer.begin(), newboundariesBuffer.begin() + newboundariesWritten);
    typename CachedVecType<uint64_t>::Writer bWriter(mergedBoundariesOut->begin(), mergedBoundariesOut->end());
    for (uint64_t idx = 0; idx < newboundariesWritten; ++idx) bWriter.write(bReader.read());
    bWriter.flush();
  }
  output.boundaries = std::move(mergedBoundariesOut);

  auto mergedCountsOut = std::make_unique<OutVecType<uint64_t>>(newcountsWritten);
  {
    typename IntmdVecType<uint64_t>::Reader cReader(newcountsBuffer.begin(), newcountsBuffer.begin() + newcountsWritten);
    typename OutVecType<uint64_t>::Writer cWriter(mergedCountsOut->begin(), mergedCountsOut->end());
    for (uint64_t idx = 0; idx < newcountsWritten; ++idx) cWriter.write(cReader.read());
    cWriter.flush();
  }
  output.counts = std::move(mergedCountsOut);

  return output;
}

template <template <class, auto...> class VecType,
          template <class, auto...> class CachedVecType,
          template <class, auto...> class IntmdVecType>
template <typename InputVecType>
void PrivDS<VecType, CachedVecType, IntmdVecType>::init(InputVecType& input, double eps_svt, double delta_svt,
                                                        double eps_op, double delta_op,
                                                        uint64_t cache_page_size) {
  if constexpr (std::is_same_v<InputVecType, StdVector<EM::Definition::Item>>) {
    EM::Primitive::BitonicSort(input);
  } else {
    EM::Primitive::KWayButterflySort(input);
  }

  const uint64_t n = input.size();
  typename InputVecType::Reader inputReader(input.begin(), input.end(), 1);
  typename InputVecType::Writer inputWriter(input.begin(), input.end(), 2);

  const EM::Definition::Item dummyItem = EM::Definition::Item();
  uint64_t lastKey = 0;
  bool hasLastKey = false;
  for (uint64_t i = 0; i < n; ++i) {
    EM::Definition::Item current = inputReader.read();
    const uint64_t currentKey = current.get_key();
    const bool isDup = hasLastKey & (currentKey == lastKey);
    obliMove(isDup, current, dummyItem);
    inputWriter.write(current);
    obliMove(!isDup, lastKey, currentKey);
    obliMove(!isDup, hasLastKey, true);
  }
  inputWriter.flush();

  const auto isMarked = [](const EM::Definition::Item& item) {
    return !item.is_dummy();
  };

  // OrCompact needs reference iterators; NonCachedVector returns by value from *it.
  // Copy to ExtVector, compact there, then copy back (Reader/Writer auth matches
  // dedup pass 2 and DOPartition/DOItemAssign pass 3).
  if constexpr (std::is_same_v<InputVecType, StdVector<EM::Definition::Item>>) {
    EM::Primitive::OrCompact(input.begin(), input.end(), isMarked);
  } else {
    using ScratchExtVec =
        EM::ExtVector::Vector<EM::Definition::Item, BLOCK_SIZE - 32, true, true, 1024>;
    ScratchExtVec scratch(n);
    {
      typename InputVecType::Reader rd(input.begin(), input.end(), 2);
      typename ScratchExtVec::Writer wr(scratch.begin(), scratch.end());
      for (uint64_t i = 0; i < n; ++i) {
        wr.write(rd.read());
      }
      wr.flush();
    }
    EM::Primitive::OrCompact(scratch.begin(), scratch.end(), isMarked);
    {
      typename ScratchExtVec::Reader rd(scratch.begin(), scratch.end());
      typename InputVecType::Writer wr(input.begin(), input.end(), 3);
      for (uint64_t i = 0; i < n; ++i) {
        wr.write(rd.read());
      }
      wr.flush();
    }
  }

  auto getKey = [](const EM::Definition::Item& item) {
    return item.get_key();
  };

  uint64_t count = 0;
  auto partitioned = DOPartition<VecType, CachedVecType, IntmdVecType>(input.begin(), input.end(), getKey,
                                         count, true, eps_svt, delta_svt, cache_page_size, 3);
  this->Z = partitioned.Z;
  this->Z_raw = partitioned.Z_raw;
  this->boundaries = std::move(partitioned.boundaries);
  this->counts = std::move(partitioned.counts);

  auto assigned_intmd = DOItemAssign<VecType, CachedVecType, IntmdVecType>(
      input.begin(), input.end(), getKey,
      *this->boundaries, *this->counts, eps_op, delta_op, this->Z, 3);
  finalize_items(std::move(assigned_intmd), input);
}

template <template <class, auto...> class VecType,
          template <class, auto...> class CachedVecType,
          template <class, auto...> class IntmdVecType>
template <template <class, auto...> class OtherVecType,
          template <class, auto...> class OtherCachedVecType,
          template <class, auto...> class OtherIntmdVecType>
void PrivDS<VecType, CachedVecType, IntmdVecType>::reconstruct(
    PrivDS<OtherVecType, OtherCachedVecType, OtherIntmdVecType>& other,
    double eps_svt, double delta_svt,
    double eps_op, double delta_op,
    uint64_t cache_page_size,
    uint64_t total_real_count, bool remove_deletions) {
  assert(this->Z > 0 && this->Z == other.Z);

  auto merged = DOPMerge<VecType, CachedVecType, IntmdVecType>(
      *this->boundaries, *other.boundaries,
      *this->items, *other.items,
      eps_svt, delta_svt, eps_op, delta_op, 
      this->Z, cache_page_size, total_real_count, remove_deletions);

  this->items.reset();
  this->boundaries.reset();
  this->counts.reset();
  other.items.reset();
  other.boundaries.reset();
  other.counts.reset();

  this->Z = merged.Z;
  this->Z_raw = merged.Z_raw;
  this->boundaries = std::move(merged.boundaries);
  this->counts = std::move(merged.counts);

  auto getKey = [](const EM::Definition::Item& item) {
    return item.get_key();
  };

  auto assigned_intmd = DOItemAssign<VecType, CachedVecType, IntmdVecType>(
      merged.items->begin(), merged.items->end(), getKey,
      *this->boundaries, *this->counts, eps_op, delta_op, this->Z);
      
  finalize_items(std::move(assigned_intmd), *merged.items);
}

template <template <class, auto...> class VecType,
          template <class, auto...> class CachedVecType,
          template <class, auto...> class IntmdVecType>
template <typename SourceToRelease>
void PrivDS<VecType, CachedVecType, IntmdVecType>::finalize_items(
    IntmdVecType<EM::Definition::Item>&& assigned_intmd,
    SourceToRelease& source_to_release) {
  if constexpr (std::is_same_v<IntmdVecType<EM::Definition::Item>, ItemVec>) {
    this->items = std::make_unique<ItemVec>(std::move(assigned_intmd));
  } else {
    { SourceToRelease drop(std::move(source_to_release)); }
    const uint64_t sz = assigned_intmd.size();
    auto result = std::make_unique<ItemVec>(sz);
    typename IntmdVecType<EM::Definition::Item>::Reader rd(
        assigned_intmd.begin(), assigned_intmd.end());
    typename ItemVec::Writer wr(result->begin(), result->end());
    for (uint64_t i = 0; i < sz; ++i) wr.write(rd.read());
    wr.flush();
    this->items = std::move(result);
  }
}

}  // namespace EM::Sapphmap