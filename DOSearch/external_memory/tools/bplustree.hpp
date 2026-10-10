#pragma once

/**
 * @file bplustree.hpp
 * @brief Static query-only B+ tree utilities for boundary navigation.
 *
 * The tree is bulk-built from sorted boundary keys and used for lower_bound
 * queries in external-memory levels. It is intentionally immutable after build
 * to minimize complexity and fit benchmark access patterns.
 */

#include <memory>

#include "common/utils.hpp"
#include "external_memory/noncachedvector.hpp"
#include "external_memory/extemvector.hpp"

// =============================================================================
// Immutable external-memory B+ tree with bulk-build + lower_bound query.
// Supports both NonCachedVector and ExtVector through a unified VecT interface.
// =============================================================================

namespace EM::Sapphmap {

// --- Constants and node layout -------------------------------------------------
// Internal node encoding uses alternating [child_offset, separator_key] slots.
// Unused slots are padded with UINT64_MAX sentinels.

constexpr uint64_t kStaticBPlusTreeMaxLevels = 64;

// --- Tree structure -----------------------------------------------------------
// leaf_data references caller-owned sorted keys; index_storage owns internal nodes.

template <typename VecT>
struct StaticBPlusTree {
  VecT& leaf_data;
  std::unique_ptr<VecT> index_storage;
  uint64_t level_start[kStaticBPlusTreeMaxLevels];
  uint64_t num_levels;
  uint64_t block_size;
  uint64_t max_keys_per_node;
  uint64_t n;
  /// Optional inclusive domain tail for synthetic final segment lookup.
  uint64_t domain_D;

  StaticBPlusTree(VecT& leaves, std::unique_ptr<VecT> index, const uint64_t* lstart,
                  uint64_t levels, uint64_t blk, uint64_t max_k, uint64_t leaf_count,
                  uint64_t domain_D_val = UINT64_MAX)
      : leaf_data(leaves),
        index_storage(std::move(index)),
        num_levels(levels),
        block_size(blk),
        max_keys_per_node(max_k),
        n(leaf_count),
        domain_D(domain_D_val) {
    for (uint64_t i = 0; i < kStaticBPlusTreeMaxLevels; ++i)
      level_start[i] = lstart[i];
  }

  uint64_t getByteSize() const {
      auto vecBytes = [](const auto& v) -> uint64_t {
          using Vec = std::decay_t<decltype(v)>;
          if constexpr (Vec::item_per_page == 1) {
              return sizeof(Vec) + v.capacity() * sizeof(typename Vec::value_type);
          } else if constexpr (!Vec::useStdCopy) {
              using Page = typename Vec::Page;
              using EncPage = typename Page::Encrypted_t;
              uint64_t num_pages = divRoundUp(v.N, Vec::item_per_page);
              return sizeof(Vec) + num_pages * sizeof(EncPage);
          } else {
              using Page = typename Vec::Page;
              using EncPage = typename Page::Encrypted_t;
              uint64_t num_pages = v.N / Vec::item_per_page + 1;
              return sizeof(Vec) + Vec::DMCacheSize * (sizeof(Page) + 16)
                     + num_pages * sizeof(EncPage);
          }
      };
      uint64_t bytes = sizeof(*this);
      // leaf_data is a reference (not owned); its memory is counted by the PrivDS owner
      if (index_storage) bytes += vecBytes(*index_storage);
      return bytes;
  }
};

// --- Query result -------------------------------------------------------------
// lower_bound semantics: first element >= k; index == UINT64_MAX if none.

struct BPlusTreeQueryResult {
  uint64_t value;   // First element >= k
  uint64_t index;   // Index in sorted_data; UINT64_MAX if not found
};

// --- Build (bulk load) -------------------------------------------------------
// sorted_data must be ascending; block_size is internal-node slot count.

template <template <class, auto...> class VectorType = EM::NonCachedVector::Vector>
inline StaticBPlusTree<VectorType<uint64_t>> build_bplus_tree(
    VectorType<uint64_t>& sorted_data, uint64_t block_size,
    uint64_t domain_D = UINT64_MAX) {
  using VecT = VectorType<uint64_t>;
  using Reader = typename VecT::Reader;
  using Writer = typename VecT::Writer;

  const uint64_t n = sorted_data.size();
  const uint64_t max_keys = (block_size >= 1) ? ((block_size - 1) >> 1) : 0;
  const uint64_t max_children = max_keys + 1;
  const uint64_t node_slots = block_size;

  if (n == 0 || block_size == 0) {
    uint64_t lstart[kStaticBPlusTreeMaxLevels] = {0};
    return StaticBPlusTree<VecT>(
        sorted_data, std::make_unique<VecT>(0), lstart, 0, block_size, max_keys, 0, domain_D);
  }

  // Compute level widths bottom-up until the root has a single node.
  const uint64_t num_leaf_blocks = divRoundUp(n, block_size);
  uint64_t num_nodes_at_level[kStaticBPlusTreeMaxLevels];
  num_nodes_at_level[0] = divRoundUp(num_leaf_blocks, max_children);
  uint64_t num_levels = 1;
  while (num_nodes_at_level[num_levels - 1] > 1) {
    num_nodes_at_level[num_levels] =
        divRoundUp(num_nodes_at_level[num_levels - 1], max_children);
    ++num_levels;
  }

  uint64_t total_slots = 0;
  for (uint64_t L = 0; L < num_levels; ++L)
    total_slots += num_nodes_at_level[L] * node_slots;

  uint64_t level_start[kStaticBPlusTreeMaxLevels];
  level_start[0] = 0;
  for (uint64_t L = 1; L < num_levels; ++L)
    level_start[L] = level_start[L - 1] + num_nodes_at_level[L - 1] * node_slots;

  auto index_storage = std::make_unique<VecT>(total_slots, static_cast<uint64_t>(UINT64_MAX));

  // first_keys_all stores one representative key per node for parent construction.
  uint64_t total_first_keys = 0;
  for (uint64_t t = 0; t + 1 < num_levels; ++t)
    total_first_keys += num_nodes_at_level[t];
  VecT first_keys_all(
      (num_levels >= 2) ? total_first_keys : 0, static_cast<uint64_t>(0));

  uint64_t seg_start[kStaticBPlusTreeMaxLevels];
  seg_start[0] = 0;
  for (uint64_t t = 1; t < num_levels; ++t)
    seg_start[t] = seg_start[t - 1] + num_nodes_at_level[t - 1];

  // --- Level 0: parent of leaf blocks ---------------------------------------
  // Use direct random access to each block's first key.
  {
    Writer w(index_storage->begin() + level_start[0],
             index_storage->begin() + level_start[1]);
    Writer w0_first(first_keys_all.begin(),
                   first_keys_all.begin() + (num_levels >= 2 ? num_nodes_at_level[0] : 0u));

    for (uint64_t node_idx = 0; node_idx < num_nodes_at_level[0]; ++node_idx) {
      const uint64_t node_start_block = node_idx * max_children;
      const uint64_t num_children =
          (node_start_block + max_children <= num_leaf_blocks)
              ? max_children
              : (num_leaf_blocks - node_start_block);
      if (num_children == 0) break;

      uint64_t key0 = *(sorted_data.begin() + (node_start_block + 0) * block_size);

      for (uint64_t i = 0; i < num_children; ++i) {
        w.write((node_start_block + i) * block_size);
        if (i + 1 < num_children) {
          uint64_t pos_next = (node_start_block + i + 1) * block_size;
          uint64_t key_next = *(sorted_data.begin() + pos_next);
          w.write(key_next);
        }
      }
      for (uint64_t written = 2 * num_children - 1; written < node_slots; ++written)
        w.write(UINT64_MAX);

      if (num_levels >= 2)
        w0_first.write(key0);
    }
    w.flush();
    if (num_levels >= 2) w0_first.flush();
  }

  // --- Upper levels to root --------------------------------------------------
  for (uint64_t L = 1; L < num_levels; ++L) {
    const uint64_t prev_level_start = level_start[L - 1];
    const uint64_t num_prev = num_nodes_at_level[L - 1];
    Reader r_prev(first_keys_all.begin() + seg_start[L - 1],
                  first_keys_all.begin() + seg_start[L - 1] + num_prev);
    Writer wL(index_storage->begin() + level_start[L],
             index_storage->begin() + level_start[L] + num_nodes_at_level[L] * node_slots);
    Writer wL_first(first_keys_all.begin() + seg_start[L],
                   first_keys_all.begin() + seg_start[L] +
                       (L + 1 < num_levels ? num_nodes_at_level[L] : 0u));

    for (uint64_t node_idx = 0; node_idx < num_nodes_at_level[L]; ++node_idx) {
      const uint64_t node_start = node_idx * max_children;
      const uint64_t num_children =
          (node_start + max_children <= num_prev)
              ? max_children
              : (num_prev - node_start);
      uint64_t key0 = r_prev.read();
      for (uint64_t i = 0; i < num_children; ++i) {
        wL.write(prev_level_start + (node_start + i) * node_slots);
        if (i + 1 < num_children) {
          uint64_t key_next = r_prev.read();
          wL.write(key_next);
        }
      }
      for (uint64_t written = 2 * num_children - 1; written < node_slots; ++written)
        wL.write(UINT64_MAX);
      if (L + 1 < num_levels)
        wL_first.write(key0);
    }
    wL.flush();
    if (L + 1 < num_levels) wL_first.flush();
  }

  uint64_t lstart_out[kStaticBPlusTreeMaxLevels];
  for (uint64_t i = 0; i < kStaticBPlusTreeMaxLevels; ++i)
    lstart_out[i] = level_start[i];
  return StaticBPlusTree<VecT>(
      sorted_data, std::move(index_storage), lstart_out,
      num_levels, block_size, max_keys, n, domain_D);
}

// --- Query (lower_bound) -------------------------------------------------------
// Returns first element >= k and its index, or UINT64_MAX when absent.

template <typename VecT>
inline BPlusTreeQueryResult lower_bound_query(StaticBPlusTree<VecT>& tree, uint64_t k) {
  constexpr uint64_t kNotFound = UINT64_MAX;
  BPlusTreeQueryResult no_result = {kNotFound, kNotFound};
  if (tree.n == 0 || tree.num_levels == 0) return no_result;

  VecT& idx = *tree.index_storage;
  const uint64_t block_size = tree.block_size;
  const uint64_t max_keys = tree.max_keys_per_node;

  // Descend root -> leaf block that may contain the lower bound.
  uint64_t child_offset = tree.level_start[tree.num_levels - 1];
  for (uint64_t L = tree.num_levels; L > 0; --L) {
    const uint64_t node_base = child_offset;
    uint64_t left = 0, right = max_keys;
    while (left < right) {
      uint64_t mid = left + (right - left) / 2;
      uint64_t key_slot = 2 * mid + 1;
      uint64_t key_val = *(idx.begin() + node_base + key_slot);
      if (key_val == UINT64_MAX)
        right = mid;
      else if (k < key_val)
        right = mid;
      else
        left = mid + 1;
    }
    child_offset = *(idx.begin() + node_base + (left << 1));
  }

  VecT& leaf = tree.leaf_data;
  uint64_t start = child_offset;
  uint64_t end = (child_offset + block_size < tree.n) ? (child_offset + block_size) : tree.n;
  if (start >= end) return no_result;

  // Binary search within candidate leaf block [start, end).
  uint64_t lo = start, hi = end;
  while (lo < hi) {
    uint64_t mid = lo + (hi - lo) / 2;
    if (*(leaf.begin() + mid) < k)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo < end)
    return BPlusTreeQueryResult{*(leaf.begin() + lo), lo};
  if (end < tree.n)
    return BPlusTreeQueryResult{*(leaf.begin() + end), end};
  if (tree.domain_D != kNotFound && k <= tree.domain_D)
    return BPlusTreeQueryResult{tree.domain_D, tree.n};
  return no_result;
}

// --- In-enclave boundary lower_bound -----------------------------------------
// Returns first j in [0, m] such that boundaries[j] >= key; returns m otherwise.

template <typename BndVec>
inline uint64_t boundary_lower_bound(const BndVec& boundaries, uint64_t key) {
  const uint64_t m = boundaries.size();
  uint64_t lo = 0, hi = m;
  while (lo < hi) {
    const uint64_t mid = lo + (hi - lo) / 2;
    if (boundaries[mid] < key)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

}  // namespace EM::Sapphmap
