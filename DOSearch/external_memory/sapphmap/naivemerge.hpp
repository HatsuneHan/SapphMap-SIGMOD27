#pragma once

#include <vector>
#include "common/utils.hpp"
#include "external_memory/extemvector.hpp"
#include "external_memory/noncachedvector.hpp"
#include "external_memory/primitive/compaction/or_compact_shuffle.hpp"
#include "external_memory/primitive/sorting/kway_butterfly_sort.hpp"
#include "external_memory/sapphmap/pds_def.hpp"
#include "external_memory/sapphmap/privpartition.hpp"

namespace EM::Sapphmap {

template <template <class, auto...> class VectorType>
struct NaiveMergeOutput {
  VectorType<EM::Definition::Item> items;
  EM::ExtVector::Vector<uint64_t> boundaries;
  VectorType<uint64_t> counts;
  uint64_t Z = 0;
};

template <template <class, auto...> class VectorType>
void NaiveMergeWithPartitionInputs(
    VectorType<EM::Definition::Item>& items1,
    VectorType<EM::Definition::Item>& items2, double eps, double delta,
    bool c_full = true, uint32_t inAuth = 0) {
  using Item = EM::Definition::Item;
  using NCVector = EM::NonCachedVector::Vector<Item>;
  (void)eps;
  (void)delta;
  (void)c_full;

  // Step 1: merge items, sort by (key asc, ver desc), and keep max ver per key.
  const uint64_t totalItems = items1.size() + items2.size();
  NCVector buffer(totalItems);
  typename NCVector::Writer bufferWriter(buffer.begin(), buffer.end(), inAuth);
  {
    typename VectorType<Item>::Reader reader(items1.begin(), items1.end());
    for (uint64_t i = 0; i < items1.size(); ++i) {
      bufferWriter.write(reader.read());
    }
  }
  {
    typename VectorType<Item>::Reader reader(items2.begin(), items2.end());
    for (uint64_t i = 0; i < items2.size(); ++i) {
      bufferWriter.write(reader.read());
    }
  }
  bufferWriter.flush();

  EM::Primitive::KWayButterflySort(buffer);

  uint64_t lastKey = 0;
  bool lastKeyValid = false;
  const Item dummyItem = Item();
  uint64_t uniqueCount = 0;
  NCVector markedBuffer(totalItems);
  typename NCVector::Reader bufferReader(buffer.begin(), buffer.end(), inAuth+1);
  typename NCVector::Writer markedWriter(markedBuffer.begin(),
                                         markedBuffer.end(), inAuth);
  for (uint64_t idx = 0; idx < totalItems; ++idx) {
    Item item = bufferReader.read();
    const uint64_t key = item.get_key();
    const bool is_dup = lastKeyValid & (key == lastKey);
    const bool is_dummy = item.is_dummy();
    const bool mark_dummy = is_dup | is_dummy;
    Item out = item;
    obliMove(mark_dummy, out, dummyItem);
    markedWriter.write(out);
    obliMove(!is_dup, lastKey, key);
    obliMove(!is_dup, lastKeyValid, true);
    uniqueCount += static_cast<uint64_t>(!mark_dummy);
  }
  markedWriter.flush();

  auto isReal = [](const Item& item) { return !item.is_dummy(); };
  EM::Primitive::DOCompact(markedBuffer.begin(), markedBuffer.end(), isReal, eps, delta);

  // DOPartition on markedBuffer
  // auto getKey = [](const Item& item) { return item.get_key(); };
  // auto partitioned = DOPartition<StdVector>(markedBuffer.begin(), markedBuffer.end(), getKey,
  //                                           uniqueCount, true, eps, delta, true);

  // Step 3: keep only non-dummy items in a new vector (resize by writing).
//   NCVector outItems(uniqueCount);
//   typename NCVector::Writer outWriter(outItems.begin(), outItems.end(), inAuth);
//   typename NCVector::Reader compactReader(
//       markedBuffer.begin(), markedBuffer.begin() + uniqueCount, inAuth);
//   for (uint64_t i = 0; i < uniqueCount; ++i) {
//     outWriter.write(compactReader.read());
//   }
//   outWriter.flush();

//   VectorType<Item> outItemsExt(uniqueCount);
//   typename VectorType<Item>::Writer outExtWriter(outItemsExt.begin(),
//                                                  outItemsExt.end());
//   typename NCVector::Reader outReader(outItems.begin(), outItems.end(), inAuth);
//   for (uint64_t i = 0; i < uniqueCount; ++i) {
//     outExtWriter.write(outReader.read());
//   }
//   outExtWriter.flush();

  return;
}

}  // namespace EM::Sapphmap
