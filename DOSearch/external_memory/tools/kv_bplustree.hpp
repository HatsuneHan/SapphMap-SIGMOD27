#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

#include "common/defs.hpp"
#include "common/utils.hpp"
#include "external_memory/extemvector.hpp"
#include "external_memory/noncachedvector.hpp"
#include "external_memory/primitive/sorting/sort_building_blocks.hpp"
#include "external_memory/stdvector.hpp"

namespace EM::Sapphmap {

// =============================================================================
// Constants
// =============================================================================

/// Each BPlusNode occupies ~4088 bytes (close to one 4KB page).
/// Layout: Header (16B) + 254 keys (2032B) + max(254 values, 255 children) (2040B).
constexpr uint32_t kMaxKeysPerNode = 254;

/// Minimum fill factor for rebalancing during Delete.
/// A node underflows when num_keys < kMinKeysPerNode.
constexpr uint32_t kMinKeysPerNode = kMaxKeysPerNode / 2;  // 127

constexpr uint64_t kNotFound = UINT64_MAX;

// =============================================================================
// BPlusNode — single node for both leaf and internal use
// =============================================================================

struct BPlusNode {
    bool is_leaf;
    uint32_t num_keys;
    uint64_t next_leaf;  // leaf-only: ID of right sibling in linked list (kNotFound = end)
    uint64_t keys[kMaxKeysPerNode];
    union {
        uint64_t values[kMaxKeysPerNode];        // leaf: payload values
        uint64_t children[kMaxKeysPerNode + 1];  // internal: child node IDs
    };

    BPlusNode() : is_leaf(true), num_keys(0), next_leaf(kNotFound) {
        for (uint32_t i = 0; i < kMaxKeysPerNode; ++i) {
            keys[i] = 0;
            values[i] = 0;
        }
        children[kMaxKeysPerNode] = 0;
    }
};

/// Trivially copyable (key, value) pair returned by range query (legacy compat).
struct BptRangeKv {
    uint64_t key;
    uint64_t value;
};

// =============================================================================
// KVBPlusTree — Baseline B+ tree with hybrid Enclave / External storage
// =============================================================================
//
// Template parameters match SapphMap for interchangeable benchmarking:
//   CachePageSize: block size for encrypted I/O pages (default 16KB)
//   EnclaveSize:   enclave memory budget in bytes (default 64MB)
//
// Storage architecture:
//   - Top levels (root + upper internals) reside in StdVector (enclave memory).
//     These are accessed on every operation and benefit from zero-overhead access.
//   - Bottom levels (lower internals + leaves) reside in ExtVector with a
//     bounded LRU cache. The cache absorbs repeated accesses to the "transition
//     layer" between enclave and external, reducing OCall/encryption overhead.
//
// Node ID encoding:
//   Bit 63 = 1 → enclave node; Bit 63 = 0 → external node.
//   Bits 0..62 = index within respective storage vector.
//   This enables O(1) dispatch without additional lookups.
//
// =============================================================================

template <uint64_t CachePageSize = BLOCK_SIZE, uint64_t EnclaveSize = ENCLAVE_SIZE>
struct KVBPlusTree {
    using Item = EM::Definition::Item;
    using OpType = EM::Definition::OpType;

    // --- Vector type aliases (consistent with SapphMap) ----------------------

    /// Non-cached external vector: optimal for sequential bulk I/O
    /// (Ingest writes, RangeQuery output). No cache memory overhead.
    template <typename T, auto...>
    using NCVec = EM::NonCachedVector::Vector<T, CachePageSize - 32, true, true>;

    /// LRU cache size for external node storage (in pages).
    ///
    /// With CachePageSize=16KB and sizeof(BPlusNode)=4088:
    ///   item_per_page = (16384-32)/4088 = 3 nodes/page (ExtVector truncates).
    ///   32 pages × 3 nodes/page = 96 nodes cached.
    ///
    /// This comfortably covers the first external level (~255 nodes at most when
    /// the top 2 tree levels are in enclave). The trade-off:
    ///   32 × sizeof(Page) ≈ 32 × 12264B ≈ 383KB of enclave memory for cache.
    ///   Negligible relative to the 64MB default enclave budget.
    ///
    /// During Ingest (sequential writes), the cache naturally holds the current
    /// write frontier. During Insert/Delete/PointQuery (random access), the LRU
    /// policy retains hot internal nodes, reducing OCalls to ~1 per operation
    /// for the external layer after warm-up.
    static constexpr uint64_t kNodeCacheSize = 32;

    /// Cached external vector for B+ tree node storage.
    template <typename T, auto...>
    using ExtCachedVec =
        EM::ExtVector::Vector<T, CachePageSize - 32, true, true, kNodeCacheSize>;

    /// In-enclave vector: direct memory, zero I/O overhead.
    template <typename T, auto...>
    using StdVec = StdVector<T>;

    // --- Node ID encoding ----------------------------------------------------

    static constexpr uint64_t kEnclaveFlag = 1ULL << 63;
    static constexpr uint64_t kInvalidId = UINT64_MAX;

    static bool is_enclave_id(uint64_t id) { return (id & kEnclaveFlag) != 0; }
    static uint64_t id_index(uint64_t id) { return id & ~kEnclaveFlag; }
    static uint64_t make_enclave_id(uint64_t idx) { return idx | kEnclaveFlag; }
    static uint64_t make_external_id(uint64_t idx) { return idx; }

    // --- Storage -------------------------------------------------------------

    /// Top-level nodes in trusted enclave memory (StdVector).
    /// Direct operator[] access, no encryption, no OCalls.
    StdVector<BPlusNode> enclave_nodes_;

    /// Bottom-level nodes in untrusted memory (ExtVector with LRU cache).
    /// Each access goes through the cache; misses trigger an encrypted page
    /// fetch from untrusted memory via OCall.
    /// Marked mutable because ExtVector::Get() updates internal cache state
    /// even for read-only lookups, but this doesn't affect logical tree state.
    mutable std::unique_ptr<ExtCachedVec<BPlusNode>> external_nodes_;

    // --- Allocators ----------------------------------------------------------

    uint64_t enclave_alloc_ = 0;
    uint64_t external_alloc_ = 0;
    uint64_t enclave_capacity_ = 0;
    uint64_t external_capacity_ = 0;

    /// Simple free lists for node recycling during Delete (merge frees nodes).
    /// Stored in enclave memory (std::vector on enclave heap); size bounded by
    /// total merges which is typically small relative to tree size.
    std::vector<uint64_t> enclave_free_;
    std::vector<uint64_t> external_free_;

    // --- Tree state ----------------------------------------------------------

    uint64_t root_id_ = kInvalidId;
    uint64_t tree_height_ = 0;  ///< 0=empty, 1=root-only leaf, H=H levels
    uint64_t size_ = 0;         ///< number of live key-value pairs

    /// Nodes at level_from_leaf >= this threshold → enclave storage.
    /// Nodes below the threshold → external storage.
    /// "Level from leaf": leaves = 0, root = tree_height_ - 1.
    uint64_t enclave_depth_threshold_ = 0;

    // =========================================================================
    // Constructor
    // =========================================================================
    //
    // @param max_nodes  Total pre-allocated capacity (enclave + external).
    //                   Must accommodate the expected tree size including
    //                   internal nodes (~N/kMaxKeys overhead for N entries).
    //
    // The constructor partitions max_nodes between enclave and external storage
    // based on EnclaveSize. Enclave budget = EnclaveSize/4 (leaving room for
    // buffer, stack, and other enclave allocations).

    explicit KVBPlusTree(uint64_t max_nodes) {
        // Reserve 1/4 of enclave for tree nodes; rest for other uses.
        const uint64_t enclave_budget = EnclaveSize / 4;
        enclave_capacity_ = enclave_budget / sizeof(BPlusNode);
        if (enclave_capacity_ > max_nodes) enclave_capacity_ = max_nodes;
        if (enclave_capacity_ == 0) enclave_capacity_ = 1;  // always room for root

        enclave_nodes_.resize(enclave_capacity_);

        external_capacity_ = (max_nodes > enclave_capacity_)
            ? (max_nodes - enclave_capacity_)
            : 0;
        if (external_capacity_ > 0)
            external_nodes_ = std::make_unique<ExtCachedVec<BPlusNode>>(
                external_capacity_);
    }

    // =========================================================================
    // Node access helpers
    // =========================================================================

    /// Read node by encoded ID.
    /// Enclave path: direct array access (1 cycle).
    /// External path: LRU cache lookup → hit: decrypted page read;
    ///                miss: OCall + decrypt + cache insert.
    BPlusNode read_node(uint64_t id) const {
        if (is_enclave_id(id))
            return enclave_nodes_[id_index(id)];
        else
            return external_nodes_->Get(id_index(id));
    }

    /// Write node by encoded ID.
    void write_node(uint64_t id, const BPlusNode& node) {
        if (is_enclave_id(id))
            enclave_nodes_[id_index(id)] = node;
        else
            external_nodes_->At(id_index(id)) = node;
    }

    /// Allocate from enclave storage (reuses freed nodes first).
    uint64_t alloc_enclave() {
        if (!enclave_free_.empty()) {
            uint64_t idx = enclave_free_.back();
            enclave_free_.pop_back();
            return make_enclave_id(idx);
        }
        if (enclave_alloc_ >= enclave_capacity_)
            throw std::runtime_error("KVBPlusTree: enclave storage exhausted");
        return make_enclave_id(enclave_alloc_++);
    }

    /// Allocate from external storage (reuses freed nodes first).
    uint64_t alloc_external() {
        if (!external_free_.empty()) {
            uint64_t idx = external_free_.back();
            external_free_.pop_back();
            return make_external_id(idx);
        }
        if (external_alloc_ >= external_capacity_)
            throw std::runtime_error("KVBPlusTree: external storage exhausted");
        return make_external_id(external_alloc_++);
    }

    /// Allocate a node at the given level_from_leaf.
    /// Levels >= enclave_depth_threshold_ → enclave (if capacity allows).
    /// Otherwise → external.  Graceful fallback: if enclave is full,
    /// the node goes to external (suboptimal but correct).
    uint64_t alloc_node(uint64_t level_from_leaf) {
        if ((level_from_leaf >= enclave_depth_threshold_ || external_capacity_ == 0) &&
            (enclave_alloc_ < enclave_capacity_ || !enclave_free_.empty())) {
            return alloc_enclave();
        }
        return alloc_external();
    }

    /// Return a node to its free list for reuse.
    void free_node(uint64_t id) {
        if (id == kInvalidId) return;
        if (is_enclave_id(id))
            enclave_free_.push_back(id_index(id));
        else
            external_free_.push_back(id_index(id));
    }

    // =========================================================================
    // Enclave / External split computation
    // =========================================================================

    /// Compute enclave_depth_threshold_ based on the THEORETICAL maximum number
    /// of nodes at each tree level (worst-case fanout = kMaxKeysPerNode + 1 = 255).
    ///
    /// The threshold guarantees that all nodes at levels >= threshold CAN fit
    /// in enclave when the tree is fully packed. After dynamic Insert/Delete,
    /// actual node counts may differ, but the allocation falls back gracefully.
    void recompute_enclave_split() {
        if (tree_height_ == 0) {
            enclave_depth_threshold_ = 0;
            return;
        }
        uint64_t cum = 0;
        uint64_t level_nodes = 1;  // root: 1 node
        uint64_t enc_levels = 0;

        for (uint64_t i = 0; i < tree_height_; ++i) {
            if (cum + level_nodes > enclave_capacity_) break;
            cum += level_nodes;
            enc_levels++;
            // Next level down has up to (kMaxKeysPerNode+1) × current nodes
            if (level_nodes > enclave_capacity_) break;  // overflow guard
            level_nodes *= (kMaxKeysPerNode + 1);
        }
        if (enc_levels == 0) enc_levels = 1;  // at minimum keep root in enclave

        enclave_depth_threshold_ = (tree_height_ > enc_levels)
            ? (tree_height_ - enc_levels)
            : 0;
    }

    // =========================================================================
    // Key-search helpers
    // =========================================================================

    /// Upper bound: returns first index i such that node.keys[i] > key.
    /// Used for internal node routing (descend to children[result]).
    static uint32_t upper_bound_keys(const BPlusNode& node, uint64_t key) {
        // Binary search on [0, num_keys)
        uint32_t lo = 0, hi = node.num_keys;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            if (node.keys[mid] <= key)
                lo = mid + 1;
            else
                hi = mid;
        }
        return lo;
    }

    /// Lower bound: returns first index i such that node.keys[i] >= key.
    /// Used for leaf search.
    static uint32_t lower_bound_keys(const BPlusNode& node, uint64_t key) {
        uint32_t lo = 0, hi = node.num_keys;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            if (node.keys[mid] < key)
                lo = mid + 1;
            else
                hi = mid;
        }
        return lo;
    }

    /// Exact-match search in a leaf. Returns index or -1 if not found.
    static int32_t find_key_in_leaf(const BPlusNode& leaf, uint64_t key) {
        uint32_t pos = lower_bound_keys(leaf, key);
        if (pos < leaf.num_keys && leaf.keys[pos] == key)
            return static_cast<int32_t>(pos);
        return -1;
    }

    // =========================================================================
    // Insert
    // =========================================================================
    //
    // Top-down recursive insert with bottom-up split propagation.
    // New nodes are allocated to the appropriate storage based on their
    // level-from-leaf. Cross-boundary splits (e.g., external leaf split
    // propagating into an enclave parent) are handled transparently via
    // the node ID encoding — read_node/write_node dispatch automatically.

    void Insert(uint64_t key, uint64_t value) {
        if (root_id_ == kInvalidId) {
            // First insert: create a root leaf.
            tree_height_ = 1;
            recompute_enclave_split();
            root_id_ = alloc_node(0);
            BPlusNode root;
            root.is_leaf = true;
            root.num_keys = 1;
            root.keys[0] = key;
            root.values[0] = value;
            root.next_leaf = kInvalidId;
            write_node(root_id_, root);
            size_ = 1;
            return;
        }

        uint64_t split_key = 0;
        uint64_t split_child = kInvalidId;
        bool did_split = insert_impl(root_id_, key, value,
                                     tree_height_ - 1,
                                     split_key, split_child);
        if (did_split) {
            // Root split → new root one level above.
            tree_height_++;
            recompute_enclave_split();
            uint64_t new_root_id = alloc_node(tree_height_ - 1);
            BPlusNode nr;
            nr.is_leaf = false;
            nr.num_keys = 1;
            nr.keys[0] = split_key;
            nr.children[0] = root_id_;
            nr.children[1] = split_child;
            write_node(new_root_id, nr);
            root_id_ = new_root_id;
        }
    }

private:
    /// Recursive insert helper.
    /// @param level  level-from-leaf of node_id (0 = leaf).
    /// @return true if this node split; out_split_key/out_split_child populated.
    bool insert_impl(uint64_t node_id, uint64_t key, uint64_t value,
                     uint64_t level,
                     uint64_t& out_split_key, uint64_t& out_split_child) {
        BPlusNode node = read_node(node_id);

        if (node.is_leaf) {
            uint32_t pos = lower_bound_keys(node, key);

            // Duplicate key → update value in-place.
            if (pos < node.num_keys && node.keys[pos] == key) {
                node.values[pos] = value;
                write_node(node_id, node);
                return false;
            }

            // Room in leaf → shift and insert.
            if (node.num_keys < kMaxKeysPerNode) {
                for (uint32_t i = node.num_keys; i > pos; --i) {
                    node.keys[i] = node.keys[i - 1];
                    node.values[i] = node.values[i - 1];
                }
                node.keys[pos] = key;
                node.values[pos] = value;
                node.num_keys++;
                write_node(node_id, node);
                size_++;
                return false;
            }

            // Leaf full → split.
            return split_leaf(node_id, node, pos, key, value,
                              out_split_key, out_split_child);
        }

        // Internal node: route to appropriate child.
        uint32_t pos = upper_bound_keys(node, key);
        uint64_t child_id = node.children[pos];
        uint64_t child_split_key, child_split_child;

        bool child_split = insert_impl(child_id, key, value,
                                       level - 1,
                                       child_split_key, child_split_child);
        if (!child_split) return false;

        // Child split → insert promoted key into this internal node.
        if (node.num_keys < kMaxKeysPerNode) {
            for (uint32_t i = node.num_keys; i > pos; --i) {
                node.keys[i] = node.keys[i - 1];
                node.children[i + 1] = node.children[i];
            }
            node.keys[pos] = child_split_key;
            node.children[pos + 1] = child_split_child;
            node.num_keys++;
            write_node(node_id, node);
            return false;
        }

        // Internal node full → split.
        return split_internal(node_id, node, pos, level,
                              child_split_key, child_split_child,
                              out_split_key, out_split_child);
    }

    /// Split a full leaf node. The new sibling is allocated at level 0 (external).
    bool split_leaf(uint64_t node_id, BPlusNode& node,
                    uint32_t insert_pos, uint64_t key, uint64_t value,
                    uint64_t& out_split_key, uint64_t& out_split_child) {
        uint64_t new_id = alloc_node(0);  // leaves at level 0 → external
        BPlusNode new_leaf;
        new_leaf.is_leaf = true;

        // Merge existing entries + new entry into overflow buffer.
        uint64_t tk[kMaxKeysPerNode + 1], tv[kMaxKeysPerNode + 1];
        for (uint32_t i = 0, j = 0; i <= kMaxKeysPerNode; ++i) {
            if (i == insert_pos) { tk[i] = key; tv[i] = value; }
            else { tk[i] = node.keys[j]; tv[i] = node.values[j]; ++j; }
        }

        uint32_t mid = (kMaxKeysPerNode + 1) / 2;
        node.num_keys = mid;
        new_leaf.num_keys = (kMaxKeysPerNode + 1) - mid;

        // Maintain leaf linked list.
        new_leaf.next_leaf = node.next_leaf;
        node.next_leaf = new_id;

        for (uint32_t i = 0; i < mid; ++i) {
            node.keys[i] = tk[i]; node.values[i] = tv[i];
        }
        for (uint32_t i = 0; i < new_leaf.num_keys; ++i) {
            new_leaf.keys[i] = tk[mid + i]; new_leaf.values[i] = tv[mid + i];
        }

        write_node(node_id, node);
        write_node(new_id, new_leaf);

        out_split_key = new_leaf.keys[0];
        out_split_child = new_id;
        size_++;
        return true;
    }

    /// Split a full internal node at the given level.
    bool split_internal(uint64_t node_id, BPlusNode& node,
                        uint32_t insert_pos, uint64_t level,
                        uint64_t ins_key, uint64_t ins_child,
                        uint64_t& out_split_key, uint64_t& out_split_child) {
        uint64_t new_id = alloc_node(level);  // same level as node
        BPlusNode new_int;
        new_int.is_leaf = false;

        uint64_t tk[kMaxKeysPerNode + 1];
        uint64_t tc[kMaxKeysPerNode + 2];
        for (uint32_t i = 0, j = 0; i <= kMaxKeysPerNode; ++i) {
            if (i == insert_pos) tk[i] = ins_key;
            else { tk[i] = node.keys[j]; ++j; }
        }
        for (uint32_t i = 0, j = 0; i <= kMaxKeysPerNode + 1; ++i) {
            if (i == insert_pos + 1) tc[i] = ins_child;
            else { tc[i] = node.children[j]; ++j; }
        }

        uint32_t mid = (kMaxKeysPerNode + 1) / 2;
        out_split_key = tk[mid];
        out_split_child = new_id;

        node.num_keys = mid;
        for (uint32_t i = 0; i < mid; ++i) {
            node.keys[i] = tk[i]; node.children[i] = tc[i];
        }
        node.children[mid] = tc[mid];

        new_int.num_keys = kMaxKeysPerNode - mid;
        for (uint32_t i = 0; i < new_int.num_keys; ++i) {
            new_int.keys[i] = tk[mid + 1 + i];
            new_int.children[i] = tc[mid + 1 + i];
        }
        new_int.children[new_int.num_keys] = tc[kMaxKeysPerNode + 1];

        write_node(node_id, node);
        write_node(new_id, new_int);
        return true;
    }

    // =========================================================================
    // Delete  (with merge / borrow rebalancing)
    // =========================================================================
    //
    // 1. Descend from root to leaf, recording the path (parent ID + child index).
    // 2. Remove key from leaf.
    // 3. If leaf underflows (num_keys < kMinKeysPerNode):
    //    a. Try borrowing from left sibling.
    //    b. Try borrowing from right sibling.
    //    c. Merge with a sibling; this may cascade parent underflow upward.
    // 4. If root collapses (internal root with 0 keys), its sole child
    //    becomes the new root (tree height decreases).
    //
    // Cross-boundary handling: siblings may reside in different storage layers
    // (one in enclave, one in external). read_node / write_node dispatch via
    // the node ID flag, so borrow/merge work identically regardless of the
    // physical storage of each node involved.

    struct PathEntry {
        uint64_t node_id;
        uint32_t child_idx;  // which children[] slot we descended through
    };

public:
    void Delete(uint64_t key) {
        if (root_id_ == kInvalidId) return;

        // --- Phase 1: Descend to leaf, recording path ---
        std::vector<PathEntry> path;
        path.reserve(tree_height_);
        uint64_t curr = root_id_;
        BPlusNode curr_node = read_node(curr);

        while (!curr_node.is_leaf) {
            uint32_t pos = upper_bound_keys(curr_node, key);
            path.push_back({curr, pos});
            curr = curr_node.children[pos];
            curr_node = read_node(curr);
        }

        // --- Phase 2: Remove key from leaf ---
        int32_t del_pos = find_key_in_leaf(curr_node, key);
        if (del_pos < 0) return;  // key not found

        for (uint32_t i = static_cast<uint32_t>(del_pos);
             i < curr_node.num_keys - 1; ++i) {
            curr_node.keys[i] = curr_node.keys[i + 1];
            curr_node.values[i] = curr_node.values[i + 1];
        }
        curr_node.num_keys--;
        write_node(curr, curr_node);
        size_--;

        // --- Phase 3: Rebalance if underflow ---
        // Root leaf special case: allow it to go empty (tree becomes empty).
        if (path.empty()) {
            if (curr_node.num_keys == 0) {
                free_node(curr);
                root_id_ = kInvalidId;
                tree_height_ = 0;
            }
            return;
        }
        if (curr_node.num_keys >= kMinKeysPerNode) return;  // no underflow

        rebalance(path, curr);
    }

private:
    /// Propagate underflow fix from the node at `underflow_id` upward.
    void rebalance(std::vector<PathEntry>& path, uint64_t underflow_id) {
        for (int p = static_cast<int>(path.size()) - 1; p >= 0; --p) {
            BPlusNode uf_node = read_node(underflow_id);
            if (uf_node.num_keys >= kMinKeysPerNode) return;

            PathEntry& pe = path[p];
            BPlusNode parent = read_node(pe.node_id);
            uint32_t ci = pe.child_idx;
            bool is_leaf = uf_node.is_leaf;

            // --- Try borrow from LEFT sibling ---
            if (ci > 0) {
                uint64_t left_id = parent.children[ci - 1];
                BPlusNode left = read_node(left_id);
                if (left.num_keys > kMinKeysPerNode) {
                    if (is_leaf)
                        leaf_borrow_left(parent, ci - 1,
                                         left, left_id, uf_node, underflow_id);
                    else
                        internal_borrow_left(parent, ci - 1,
                                             left, left_id, uf_node, underflow_id);
                    write_node(pe.node_id, parent);
                    return;  // borrow never cascades
                }
            }

            // --- Try borrow from RIGHT sibling ---
            if (ci < parent.num_keys) {
                uint64_t right_id = parent.children[ci + 1];
                BPlusNode right = read_node(right_id);
                if (right.num_keys > kMinKeysPerNode) {
                    if (is_leaf)
                        leaf_borrow_right(parent, ci,
                                          uf_node, underflow_id, right, right_id);
                    else
                        internal_borrow_right(parent, ci,
                                              uf_node, underflow_id, right, right_id);
                    write_node(pe.node_id, parent);
                    return;
                }
            }

            // --- Must merge ---
            if (ci > 0) {
                // Merge underflow node into left sibling.
                uint64_t left_id = parent.children[ci - 1];
                BPlusNode left = read_node(left_id);
                if (is_leaf)
                    leaf_merge(left, left_id, uf_node, underflow_id);
                else
                    internal_merge(left, left_id, uf_node, underflow_id,
                                   parent.keys[ci - 1]);
                free_node(underflow_id);

                // Remove separator key[ci-1] and children[ci] from parent.
                for (uint32_t i = ci - 1; i < parent.num_keys - 1; ++i)
                    parent.keys[i] = parent.keys[i + 1];
                for (uint32_t i = ci; i < parent.num_keys; ++i)
                    parent.children[i] = parent.children[i + 1];
                parent.num_keys--;
            } else {
                // Merge right sibling into underflow node.
                uint64_t right_id = parent.children[ci + 1];
                BPlusNode right = read_node(right_id);
                if (is_leaf)
                    leaf_merge(uf_node, underflow_id, right, right_id);
                else
                    internal_merge(uf_node, underflow_id, right, right_id,
                                   parent.keys[ci]);
                free_node(right_id);

                // Remove separator key[ci] and children[ci+1] from parent.
                for (uint32_t i = ci; i < parent.num_keys - 1; ++i)
                    parent.keys[i] = parent.keys[i + 1];
                for (uint32_t i = ci + 1; i < parent.num_keys; ++i)
                    parent.children[i] = parent.children[i + 1];
                parent.num_keys--;
            }
            write_node(pe.node_id, parent);

            // Check root collapse: internal root with 0 keys → sole child is new root.
            if (p == 0 && parent.num_keys == 0) {
                uint64_t new_root = parent.children[0];
                free_node(pe.node_id);
                root_id_ = new_root;
                tree_height_--;
                recompute_enclave_split();
                return;
            }

            // Continue checking parent for underflow.
            underflow_id = pe.node_id;
        }
    }

    // --- Leaf borrow / merge helpers -----------------------------------------

    /// Borrow the rightmost entry from left sibling into underflowed leaf.
    void leaf_borrow_left(BPlusNode& parent, uint32_t sep_idx,
                          BPlusNode& left, uint64_t left_id,
                          BPlusNode& node, uint64_t node_id) {
        // Shift node entries right to make room at position 0.
        for (uint32_t i = node.num_keys; i > 0; --i) {
            node.keys[i] = node.keys[i - 1];
            node.values[i] = node.values[i - 1];
        }
        // Move rightmost entry from left.
        node.keys[0] = left.keys[left.num_keys - 1];
        node.values[0] = left.values[left.num_keys - 1];
        node.num_keys++;
        left.num_keys--;

        // Parent separator becomes the new first key of node.
        parent.keys[sep_idx] = node.keys[0];

        write_node(left_id, left);
        write_node(node_id, node);
    }

    /// Borrow the leftmost entry from right sibling into underflowed leaf.
    void leaf_borrow_right(BPlusNode& parent, uint32_t sep_idx,
                           BPlusNode& node, uint64_t node_id,
                           BPlusNode& right, uint64_t right_id) {
        node.keys[node.num_keys] = right.keys[0];
        node.values[node.num_keys] = right.values[0];
        node.num_keys++;

        for (uint32_t i = 0; i < right.num_keys - 1; ++i) {
            right.keys[i] = right.keys[i + 1];
            right.values[i] = right.values[i + 1];
        }
        right.num_keys--;

        parent.keys[sep_idx] = right.keys[0];

        write_node(node_id, node);
        write_node(right_id, right);
    }

    /// Merge right leaf into left leaf. Caller frees right.
    void leaf_merge(BPlusNode& left, uint64_t left_id,
                    BPlusNode& right, uint64_t /*right_id*/) {
        for (uint32_t i = 0; i < right.num_keys; ++i) {
            left.keys[left.num_keys + i] = right.keys[i];
            left.values[left.num_keys + i] = right.values[i];
        }
        left.num_keys += right.num_keys;
        left.next_leaf = right.next_leaf;  // skip over right in linked list
        write_node(left_id, left);
    }

    // --- Internal node borrow / merge helpers --------------------------------

    /// Borrow from left sibling: rotate through parent separator.
    void internal_borrow_left(BPlusNode& parent, uint32_t sep_idx,
                              BPlusNode& left, uint64_t left_id,
                              BPlusNode& node, uint64_t node_id) {
        // Shift node entries right.
        for (uint32_t i = node.num_keys; i > 0; --i) {
            node.keys[i] = node.keys[i - 1];
            node.children[i + 1] = node.children[i];
        }
        node.children[1] = node.children[0];

        // Parent separator → node's first key.
        node.keys[0] = parent.keys[sep_idx];
        // Left's rightmost child → node's leftmost child.
        node.children[0] = left.children[left.num_keys];
        node.num_keys++;

        // Left's rightmost key → parent separator.
        parent.keys[sep_idx] = left.keys[left.num_keys - 1];
        left.num_keys--;

        write_node(left_id, left);
        write_node(node_id, node);
    }

    /// Borrow from right sibling: rotate through parent separator.
    void internal_borrow_right(BPlusNode& parent, uint32_t sep_idx,
                               BPlusNode& node, uint64_t node_id,
                               BPlusNode& right, uint64_t right_id) {
        node.keys[node.num_keys] = parent.keys[sep_idx];
        node.children[node.num_keys + 1] = right.children[0];
        node.num_keys++;

        parent.keys[sep_idx] = right.keys[0];

        right.children[0] = right.children[1];
        for (uint32_t i = 0; i < right.num_keys - 1; ++i) {
            right.keys[i] = right.keys[i + 1];
            right.children[i + 1] = right.children[i + 2];
        }
        right.num_keys--;

        write_node(node_id, node);
        write_node(right_id, right);
    }

    /// Merge right internal into left internal, pulling down parent separator.
    void internal_merge(BPlusNode& left, uint64_t left_id,
                        BPlusNode& right, uint64_t /*right_id*/,
                        uint64_t separator_key) {
        // Pull separator down as a key in left.
        left.keys[left.num_keys] = separator_key;
        left.children[left.num_keys + 1] = right.children[0];
        left.num_keys++;

        for (uint32_t i = 0; i < right.num_keys; ++i) {
            left.keys[left.num_keys + i] = right.keys[i];
            left.children[left.num_keys + i + 1] = right.children[i + 1];
        }
        left.num_keys += right.num_keys;

        write_node(left_id, left);
    }

    // =========================================================================
    // Ingest  (BulkLoad — static bottom-up construction)
    // =========================================================================
    //
    // Called ONLY on an empty tree. Accepts a vector of KvPair (key, value)
    // sorted by key. This is NOT the same as SapphMap::Ingest which builds a
    // differential-privacy PDS; this version constructs a plain, performance-
    // optimized B+ tree — the Baseline.
    //
    // Algorithm:
    //   1. Read & sort input by key, deduplicate (keep last value per key).
    //   2. Compute tree structure: leaf count → internal node count per level.
    //   3. Determine optimal enclave/external split based on actual node counts
    //      (not worst-case fanout), maximizing nodes in enclave.
    //   4. Allocate exact storage for both layers.
    //   5. Build bottom-up: leaves → level-1 internals → ... → root.
    //      Node IDs are assigned sequentially per layer, encoded with the
    //      enclave/external flag.
    //
    // Complexity: O(N) IOs (sequential writes); all reads are from in-memory
    // sorted buffer. The LRU cache is well-utilized since external writes are
    // sequential (pages are evicted in FIFO order).

public:
    template <typename DatasetVec>
    void Ingest(DatasetVec& dataset) {
        if (dataset.size() == 0) return;

        // --- Step 1: Copy to external memory and sort ---
        // Uses ExtMergeSort which sorts in external (untrusted) memory with
        // small in-enclave batches, avoiding enclave heap overflow for large N.
        const uint64_t n_input = dataset.size();
        using SortKv = EM::Sapphmap::KvPair;  // has operator< (key-first)
        NCVec<SortKv> sort_vec(n_input);
        {
            typename DatasetVec::Reader reader(dataset.begin(), dataset.end());
            typename NCVec<SortKv>::Writer writer(sort_vec.begin(), sort_vec.end());
            for (uint64_t i = 0; i < n_input; ++i)
                writer.write(reader.read());
            writer.flush();
        }

        // Sort budget: 1/8 of enclave, leaving room for enclave_nodes_ etc.
        // incAuth=false: keep auth counter at 0 so subsequent Readers work.
        const uint64_t sort_heap = EnclaveSize / 8;
        EM::Primitive::ExtMergeSort<false>(sort_vec, sort_heap);

        // --- Step 2: Count unique keys (streaming pass) ---
        // Dedup semantics: keep the LAST value for each key (same as before).
        uint64_t N = 0;
        {
            typename NCVec<SortKv>::Reader reader(sort_vec.begin(), sort_vec.end());
            uint64_t prev_key = UINT64_MAX;
            for (uint64_t i = 0; i < n_input; ++i) {
                SortKv kv = reader.read();
                if (kv.key != prev_key) {
                    N++;
                    prev_key = kv.key;
                }
            }
        }
        if (N == 0) return;
        size_ = N;

        // --- Step 3: Compute tree structure ---
        const uint32_t max_children = kMaxKeysPerNode + 1;  // 255
        const uint64_t num_leaves = divRoundUp(N, kMaxKeysPerNode);

        std::vector<uint64_t> level_count;
        {
            uint64_t prev = num_leaves;
            while (prev > 1) {
                uint64_t cnt = divRoundUp(prev, max_children);
                level_count.push_back(cnt);
                prev = cnt;
            }
            if (num_leaves == 1 && level_count.empty()) {
                // Single leaf is the root; no internal levels needed.
            } else if (level_count.empty()) {
                level_count.push_back(1);  // at least a root internal node
            }
        }
        const uint64_t num_internal_levels = level_count.size();
        tree_height_ = num_internal_levels + 1;  // +1 for leaf level

        // --- Step 4: Optimal enclave/external split ---
        uint64_t enc_actual_count = 0;
        uint64_t enc_top_levels = 0;
        {
            for (int64_t i = static_cast<int64_t>(level_count.size()) - 1;
                 i >= 0; --i) {
                if (enc_actual_count + level_count[i] <= enclave_capacity_) {
                    enc_actual_count += level_count[i];
                    enc_top_levels++;
                } else {
                    break;
                }
            }
            if (enc_top_levels == num_internal_levels &&
                enc_actual_count + num_leaves <= enclave_capacity_) {
                enc_actual_count += num_leaves;
                enc_top_levels++;
            }
        }

        enclave_depth_threshold_ = (tree_height_ > enc_top_levels)
            ? (tree_height_ - enc_top_levels)
            : 0;

        uint64_t ext_count = num_leaves;
        for (uint64_t i = 0; i < num_internal_levels; ++i) ext_count += level_count[i];
        ext_count -= enc_actual_count;

        if (enc_actual_count > enclave_capacity_)
            enc_actual_count = enclave_capacity_;
        if (ext_count > 0 && (!external_nodes_ || ext_count > external_capacity_)) {
            external_capacity_ = ext_count + ext_count / 4 + 256;
            external_nodes_ = std::make_unique<ExtCachedVec<BPlusNode>>(
                external_capacity_);
        }

        enclave_alloc_ = 0;
        external_alloc_ = 0;
        enclave_free_.clear();
        external_free_.clear();

        // --- Step 5: Build leaves from sorted data (streaming with dedup) ---
        const bool leaves_in_enclave = (enclave_depth_threshold_ == 0);
        std::vector<uint64_t> child_ids(num_leaves);
        std::vector<uint64_t> child_first_keys(num_leaves);

        {
            typename NCVec<SortKv>::Reader reader(sort_vec.begin(), sort_vec.end());
            uint64_t input_remaining = n_input;

            // Dedup state: hold one entry, advance past same-key duplicates.
            SortKv held;
            bool has_held = false;

            // Returns next unique KvPair (keeps last value per key).
            auto next_unique = [&]() -> SortKv {
                if (!has_held) {
                    held = reader.read();
                    input_remaining--;
                }
                while (input_remaining > 0) {
                    SortKv next = reader.read();
                    input_remaining--;
                    if (next.key != held.key) {
                        SortKv result = held;
                        held = next;
                        has_held = true;
                        return result;
                    }
                    held = next;  // same key, keep later value
                }
                has_held = false;
                return held;
            };

            for (uint64_t leaf_idx = 0; leaf_idx < num_leaves; ++leaf_idx) {
                BPlusNode leaf;
                leaf.is_leaf = true;
                const uint64_t start = leaf_idx * kMaxKeysPerNode;
                const uint64_t end = (start + kMaxKeysPerNode < N)
                    ? (start + kMaxKeysPerNode) : N;
                leaf.num_keys = static_cast<uint32_t>(end - start);

                for (uint32_t i = 0; i < leaf.num_keys; ++i) {
                    SortKv kv = next_unique();
                    leaf.keys[i] = kv.key;
                    leaf.values[i] = kv.value;
                }

                uint64_t leaf_id = leaves_in_enclave
                    ? alloc_enclave() : alloc_external();
                child_ids[leaf_idx] = leaf_id;
                child_first_keys[leaf_idx] = leaf.keys[0];
                leaf.next_leaf = kInvalidId;
                write_node(leaf_id, leaf);
            }
        }

        // Set next_leaf pointers on leaves.
        for (uint64_t i = 0; i < num_leaves - 1; ++i) {
            BPlusNode leaf = read_node(child_ids[i]);
            leaf.next_leaf = child_ids[i + 1];
            write_node(child_ids[i], leaf);
        }

        // --- Step 6: Build internal levels bottom-up ---
        for (uint64_t lev = 0; lev < num_internal_levels; ++lev) {
            const uint64_t lfl = lev + 1;
            const uint64_t num_children = child_ids.size();
            const uint64_t num_parents = level_count[lev];
            const bool in_enclave = (lfl >= enclave_depth_threshold_);

            std::vector<uint64_t> parent_ids(num_parents);
            std::vector<uint64_t> parent_first_keys(num_parents);

            for (uint64_t node_idx = 0; node_idx < num_parents; ++node_idx) {
                const uint64_t c_start = node_idx * max_children;
                const uint64_t c_end = (c_start + max_children < num_children)
                    ? (c_start + max_children) : num_children;
                const uint32_t nc = static_cast<uint32_t>(c_end - c_start);

                BPlusNode inode;
                inode.is_leaf = false;
                inode.num_keys = nc - 1;
                for (uint32_t i = 0; i < nc; ++i)
                    inode.children[i] = child_ids[c_start + i];
                for (uint32_t i = 0; i < nc - 1; ++i)
                    inode.keys[i] = child_first_keys[c_start + i + 1];

                uint64_t nid = in_enclave ? alloc_enclave() : alloc_external();
                parent_ids[node_idx] = nid;
                parent_first_keys[node_idx] = child_first_keys[c_start];
                write_node(nid, inode);
            }

            child_ids = std::move(parent_ids);
            child_first_keys = std::move(parent_first_keys);
        }

        root_id_ = child_ids[0];
    }

    // =========================================================================
    // PointQuery
    // =========================================================================
    //
    // Traverse from root to leaf using binary search at each internal node.
    // Access pattern: root (enclave) → upper internals (enclave) →
    //   transition-layer internal (external, likely cached) → leaf (external).
    // Returns an Item with OpType::INS if found, or a dummy Item if not.
    // This matches SapphMap::PointQuery semantics.

    Item PointQuery(uint64_t key) {
        if (root_id_ == kInvalidId)
            return Item();  // dummy

        uint64_t curr = root_id_;
        BPlusNode node = read_node(curr);

        // Descend to leaf.
        while (!node.is_leaf) {
            uint32_t pos = upper_bound_keys(node, key);
            curr = node.children[pos];
            node = read_node(curr);
        }

        // Binary search within leaf.
        int32_t pos = find_key_in_leaf(node, key);
        if (pos >= 0)
            return Item(OpType::INS, key, node.values[pos], 0);

        return Item();  // not found → dummy
    }

    // =========================================================================
    // RangeQuery
    // =========================================================================
    //
    // Returns all (key, value) pairs with key in [lo, hi], packed into an
    // NCVec<Item>. This matches SapphMap::RangeQuery's return type.
    //
    // Algorithm:
    //   1. Descend to the leaf containing the first key >= lo.
    //   2. Scan rightward through the leaf linked list, collecting entries
    //      with key <= hi.
    //
    // Two passes: first counts entries (to size the output vector), then
    // collects them. Both passes use the linked list. For the external
    // leaf layer, this is sequential I/O which is cache-friendly.

    NCVec<Item> RangeQuery(uint64_t lo, uint64_t hi) {
        if (root_id_ == kInvalidId)
            return NCVec<Item>(0);

        // --- Pass 1: Count matching entries ---
        uint64_t start_leaf = find_leaf_for_key(lo);
        uint64_t count = 0;
        {
            uint64_t leaf_id = start_leaf;
            while (leaf_id != kInvalidId) {
                BPlusNode leaf = read_node(leaf_id);
                if (!leaf.is_leaf) break;
                uint32_t i = lower_bound_keys(leaf, lo);
                for (; i < leaf.num_keys; ++i) {
                    if (leaf.keys[i] > hi) goto count_done;
                    ++count;
                }
                leaf_id = leaf.next_leaf;
            }
        }
    count_done:

        // --- Pass 2: Collect results ---
        NCVec<Item> output(count);
        if (count == 0) return output;

        typename NCVec<Item>::Writer writer(output.begin(), output.end());
        {
            uint64_t leaf_id = start_leaf;
            while (leaf_id != kInvalidId) {
                BPlusNode leaf = read_node(leaf_id);
                if (!leaf.is_leaf) break;
                uint32_t i = lower_bound_keys(leaf, lo);
                for (; i < leaf.num_keys; ++i) {
                    if (leaf.keys[i] > hi) {
                        writer.flush();
                        return output;
                    }
                    writer.write(Item(OpType::INS, leaf.keys[i],
                                      leaf.values[i], 0));
                }
                leaf_id = leaf.next_leaf;
            }
        }
        writer.flush();
        return output;
    }

    // --- Query helpers -------------------------------------------------------

    /// Find the leaf node that would contain `key` (or the first key >= key).
    /// Uses the same upper_bound routing as PointQuery for correctness:
    /// descend to children[i] where i = first index with keys[i] > key.
    uint64_t find_leaf_for_key(uint64_t key) const {
        uint64_t curr = root_id_;
        BPlusNode node = read_node(curr);
        while (!node.is_leaf) {
            uint32_t pos = upper_bound_keys(node, key);
            curr = node.children[pos];
            node = read_node(curr);
        }
        return curr;
    }

    // =========================================================================
    // Utility
    // =========================================================================

    /// Number of live key-value pairs in the tree.
    uint64_t size() const { return size_; }

    /// Total allocated node count (enclave + external, excluding freed).
    uint64_t allocated_node_count() const {
        return enclave_alloc_ + external_alloc_;
    }

    /// Tree height (0 = empty, 1 = root leaf only, etc.)
    uint64_t height() const { return tree_height_; }

    /// Number of enclave-resident nodes currently allocated.
    uint64_t enclave_node_count() const {
        return enclave_alloc_ - enclave_free_.size();
    }

    /// Number of external-resident nodes currently allocated.
    uint64_t external_node_count() const {
        return external_alloc_ - external_free_.size();
    }

    uint64_t getByteSize() const {
        auto vecBytes = [](const auto& v) -> uint64_t {
            using Vec = std::decay_t<decltype(v)>;
            if constexpr (!Vec::useStdCopy) {
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
        bytes += enclave_nodes_.capacity() * sizeof(BPlusNode);
        if (external_nodes_) bytes += vecBytes(*external_nodes_);
        bytes += enclave_free_.capacity() * sizeof(uint64_t);
        bytes += external_free_.capacity() * sizeof(uint64_t);
        return std::round(static_cast<double>(bytes) * 100.0) / 100.0;
    }

    // =========================================================================
    // Legacy compatibility wrappers
    // =========================================================================
    // These mirror the old DynamicKVBPlusTree interface for test compatibility.

    /// Insert or update a key-value pair (alias for Insert).
    void put(uint64_t key, uint64_t value) { Insert(key, value); }

    /// Point query returning bool + out_value (alias for PointQuery).
    bool get(uint64_t key, uint64_t& out_value) {
        Item item = PointQuery(key);
        if (item.is_dummy()) return false;
        out_value = item.get_value();
        return true;
    }

    /// Delete a key (alias for Delete). Returns true if key was present.
    bool remove(uint64_t key) {
        uint64_t old_size = size_;
        Delete(key);
        return size_ < old_size;
    }

    /// Range query returning NCVec<BptRangeKv> (legacy interface).
    NCVec<BptRangeKv> range_query(uint64_t lo, uint64_t hi) {
        if (root_id_ == kInvalidId)
            return NCVec<BptRangeKv>(0);

        // Pass 1: count
        uint64_t start_leaf = find_leaf_for_key(lo);
        uint64_t count = 0;
        {
            uint64_t leaf_id = start_leaf;
            while (leaf_id != kInvalidId) {
                BPlusNode leaf = read_node(leaf_id);
                if (!leaf.is_leaf) break;
                uint32_t i = lower_bound_keys(leaf, lo);
                for (; i < leaf.num_keys; ++i) {
                    if (leaf.keys[i] > hi) goto rq_count_done;
                    ++count;
                }
                leaf_id = leaf.next_leaf;
            }
        }
    rq_count_done:

        // Pass 2: collect
        NCVec<BptRangeKv> output(count);
        if (count == 0) return output;
        typename NCVec<BptRangeKv>::Writer writer(output.begin(), output.end());
        {
            uint64_t leaf_id = start_leaf;
            while (leaf_id != kInvalidId) {
                BPlusNode leaf = read_node(leaf_id);
                if (!leaf.is_leaf) break;
                uint32_t i = lower_bound_keys(leaf, lo);
                for (; i < leaf.num_keys; ++i) {
                    if (leaf.keys[i] > hi) {
                        writer.flush();
                        return output;
                    }
                    writer.write(BptRangeKv{leaf.keys[i], leaf.values[i]});
                }
                leaf_id = leaf.next_leaf;
            }
        }
        writer.flush();
        return output;
    }
};

}  // namespace EM::Sapphmap
