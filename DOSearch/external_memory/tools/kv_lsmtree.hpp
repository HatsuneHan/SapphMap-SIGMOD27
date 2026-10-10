#pragma once

/**
 * @file kv_lsmtree.hpp
 * @brief KVLSMTree baseline index for external-memory experiments.
 *
 * KVLSMTree is a non-oblivious baseline designed for empirical comparison with
 * SapphMap. It combines an in-enclave ordered buffer with leveled sorted runs,
 * optional enclave-resident upper levels, fence pointers, block Bloom filters,
 * and a static B+ tree for external level navigation.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>
#include <set>

#include "common/defs.hpp"
#include "common/utils.hpp"
#include "external_memory/extemvector.hpp"
#include "external_memory/noncachedvector.hpp"
#include "external_memory/primitive/sorting/sort_building_blocks.hpp"
#include "external_memory/sapphmap/pds_def.hpp"
#include "external_memory/sapphmap/sapphmap.hpp"
#include "external_memory/stdvector.hpp"
#include "external_memory/tools/bplustree.hpp"

namespace EM::Sapphmap {

// --- External level boundary index constants ---

static constexpr uint64_t kKvBPlusTreePageSize     = 4096;
static constexpr uint64_t kKvBPlusBoundaryCache    = 1024;
static constexpr uint64_t kKvLevelBPlusTreeBlockSize =
    (kKvBPlusTreePageSize - 32) / sizeof(uint64_t);

template <typename T, auto... Args>
using KvBndCachedVec =
    EM::ExtVector::Vector<T, kKvBPlusTreePageSize - 32, true, true, kKvBPlusBoundaryCache>;

using KvExtBPlusTree = StaticBPlusTree<KvBndCachedVec<uint64_t>>;

/// Locate the candidate page for @p key using the external B+ tree leaf keys.
template <typename VecT>
inline uint64_t kv_lsmtree_bptree_find_page(StaticBPlusTree<VecT>& tree, uint64_t key) {
    // ExtVector::begin() is non-const; leaf is only read here via Reader.
    VecT& fk = tree.leaf_data;
    const uint64_t n = fk.size();
    if (n == 0) return 0;

    BPlusTreeQueryResult qr = lower_bound_query(tree, key);
    if (qr.index == UINT64_MAX || qr.index >= n) return n - 1;

    typename VecT::Reader r(fk.begin() + qr.index, fk.begin() + qr.index + 1);
    const uint64_t fk_at_i = r.read();
    if (fk_at_i == key) return qr.index;
    return (qr.index == 0) ? 0 : (qr.index - 1);
}


// =============================================================================
// FencePointers: lightweight in-enclave block index for external levels.
// =============================================================================

/**
 * @brief Block-level fence metadata for one sorted run.
 */
struct FencePointers {
    std::vector<uint64_t> first_keys_;
    uint64_t items_per_page_ = 0;
    /// Last key of the sorted run (set when the level is built); enables no-I/O max bound.
    uint64_t last_key_ = 0;
    bool has_last_key_ = false;

    void clear() {
        first_keys_.clear();
        items_per_page_ = 0;
        has_last_key_ = false;
    }

    void init(uint64_t items_per_page) {
        first_keys_.clear();
        items_per_page_ = items_per_page;
        has_last_key_ = false;
    }

    void record(uint64_t out_idx, uint64_t key) {
        if (items_per_page_ > 0 && out_idx % items_per_page_ == 0)
            first_keys_.push_back(key);
    }

    void set_last_key(uint64_t key) {
        last_key_ = key;
        has_last_key_ = true;
    }

    /// Returns the page index whose range contains `key`.
    /// Page p covers keys from first_keys_[p] to first_keys_[p+1)-1.
    uint64_t find_page(uint64_t key) const {
        if (first_keys_.empty()) return 0;
        auto it = std::upper_bound(first_keys_.begin(), first_keys_.end(), key);
        if (it == first_keys_.begin()) return 0;
        return static_cast<uint64_t>(std::prev(it) - first_keys_.begin());
    }

    uint64_t getByteSize() const {
        uint64_t bytes = sizeof(*this);
        bytes += first_keys_.capacity() * sizeof(uint64_t);
        return bytes;
    }

    /// True if [lo, hi] cannot intersect this level's key range (uses only fence metadata; no I/O).
    bool disjoint_from_query(uint64_t lo, uint64_t hi) const {
        if (first_keys_.empty()) return false;
        if (hi < first_keys_.front()) return true;
        if (has_last_key_ && lo > last_key_) return true;
        return false;
    }
};

// =============================================================================
// KVLSMTree: non-oblivious baseline used in benchmark comparisons.
// =============================================================================

template <uint64_t CachePageSize = BLOCK_SIZE, uint64_t EnclaveSize = ENCLAVE_SIZE>
/**
 * @brief LSM-style key-value index baseline with enclave/external hybrid levels.
 */
struct KVLSMTree {
    using Item = EM::Definition::Item;
    using OpType = EM::Definition::OpType;

    template <typename T, auto...>
    using NCVec = EM::NonCachedVector::Vector<T, CachePageSize - 32, true, true>;

    template <typename T, auto...>
    using StdVec = StdVector<T>;

    /// Same dual representation as SapphMap::RangeQueryResult (enclave vs external).
    using RangeQueryResult =
        typename SapphMap<CachePageSize, EnclaveSize>::RangeQueryResult;

    /// Dense logical items per page: enclave run strip + RangeQuery page alignment.
    /// `NCVec<Item>::item_per_page` is still NonCachedVector's formula (independent).
    static constexpr uint64_t kItemsPerPage = (CachePageSize - 32) / sizeof(Item);

    /// External "block" aligns with logical page (kItemsPerPage); avoids large read amplification.
    static constexpr uint64_t kBlockFactor   = 1;
    static constexpr uint64_t kItemsPerBlock = kItemsPerPage * kBlockFactor;

    static constexpr uint64_t kBFBitsPerBlock  = kItemsPerBlock * 10;
    static constexpr uint64_t kBFWordsPerBlock = (kBFBitsPerBlock + 63) / 64;
    static constexpr uint64_t kBFNumHashes     = 7;

    struct BlockBloomFilter {
        uint64_t bits[kBFWordsPerBlock];

        void clear() {
            for (uint64_t i = 0; i < kBFWordsPerBlock; ++i)
                bits[i] = 0;
        }

        static uint64_t mix(uint64_t k) {
            k ^= k >> 33;
            k *= 0xFF51AFD7ED558CCDULL;
            k ^= k >> 33;
            k *= 0xC4CEB9FE1A85EC53ULL;
            k ^= k >> 33;
            return k;
        }

        void add(uint64_t key) {
            uint64_t h1 = mix(key);
            uint64_t h2 = mix(key ^ 0x9E3779B97F4A7C15ULL);
            for (uint64_t i = 0; i < kBFNumHashes; ++i) {
                uint64_t pos = (h1 + i * h2) % kBFBitsPerBlock;
                bits[pos / 64] |= (1ULL << (pos % 64));
            }
        }

        bool maybe_contains(uint64_t key) const {
            uint64_t h1 = mix(key);
            uint64_t h2 = mix(key ^ 0x9E3779B97F4A7C15ULL);
            for (uint64_t i = 0; i < kBFNumHashes; ++i) {
                uint64_t pos = (h1 + i * h2) % kBFBitsPerBlock;
                if (!(bits[pos / 64] & (1ULL << (pos % 64))))
                    return false;
            }
            return true;
        }
    };

    static_assert(std::is_trivially_copyable_v<BlockBloomFilter>,
                  "BlockBloomFilter must be trivially copyable for NCVec storage");

    using BloomVec =
        EM::NonCachedVector::Vector<BlockBloomFilter, CachePageSize - 32, true, false>;

    static constexpr double kEnclaveOverheadFactor = 2.5;

    /// External levels: B+ tree over block first keys (same bulk-load query as SapphMap).
    using ExtFenceKeyVec = KvBndCachedVec<uint64_t>;
    using ExtBPlusTreeType = KvExtBPlusTree;

    struct LevelState {
        bool occupied = false;
        bool is_enclave = false;
        uint64_t real_count = 0;
        uint64_t ncapacity = 0;
        std::unique_ptr<StdVector<Item>> enc_items;
        std::unique_ptr<NCVec<Item>> ext_items;
        std::unique_ptr<BloomVec> ext_blooms;
        FencePointers fences;
        /// Sorted first key per block (external); leaf layer for ext_bptree. Declared before ext_bptree.
        std::unique_ptr<ExtFenceKeyVec> ext_fence_keys;
        std::unique_ptr<ExtBPlusTreeType> ext_bptree;

        void clear() {
            occupied = false;
            is_enclave = false;
            real_count = 0;
            enc_items.reset();
            ext_items.reset();
            ext_blooms.reset();
            ext_bptree.reset();
            ext_fence_keys.reset();
            fences.clear();
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
            if (enc_items) bytes += vecBytes(*enc_items);
            if (ext_items) bytes += vecBytes(*ext_items);
            if (ext_blooms) {
                using BPage = typename BloomVec::Page;
                using BEncPage = typename BPage::Encrypted_t;
                uint64_t npg = divRoundUp(ext_blooms->N, BloomVec::item_per_page);
                bytes += sizeof(BloomVec) + npg * sizeof(BEncPage);
            }
            // Delegate to nested structures and subtract already-accounted base size.
            bytes += fences.getByteSize() - sizeof(FencePointers);
            if (ext_fence_keys) bytes += vecBytes(*ext_fence_keys);
            if (ext_bptree) bytes += ext_bptree->getByteSize();
            return bytes;
        }
    };

    struct RunData {
        std::unique_ptr<NCVec<Item>> items;
        uint64_t count = 0;
    };

    uint64_t gamma;
    uint64_t buffer_size;
    uint64_t buffer_real_count;
    uint64_t depth;
    uint64_t enclave_levels;
    std::set<Item> buffer;
    std::vector<LevelState> levels;
    uint64_t next_ver_ = 0;

    KVLSMTree(uint64_t _buffer_size, uint64_t _gamma, uint64_t _max_writes) {
        configure(_buffer_size, _gamma, _max_writes);
    }

    void configure(uint64_t _buffer_size, uint64_t _gamma, uint64_t _max_writes) {
        buffer_size = _buffer_size;
        gamma = _gamma;

        buffer_real_count = static_cast<uint64_t>(buffer_size /
            (sizeof(Item) * kEnclaveOverheadFactor));
        if (buffer_real_count == 0) buffer_real_count = 1;

        const double level_log =
            obliLog(static_cast<double>(_max_writes) /
                    static_cast<double>(buffer_real_count)) /
            obliLog(static_cast<double>(gamma));
        const uint64_t max_level = obliFloorU64(level_log) + 1;

        depth = 0;
        next_ver_ = 0;
        buffer.clear();
        levels.clear();
        levels.resize(max_level + 1);
        uint64_t cap = buffer_real_count;
        for (uint64_t i = 0; i <= max_level; ++i) {
            levels[i].ncapacity = cap;
            cap *= gamma;
        }

        uint64_t initial_units =
            (buffer_size > 0) ? (EnclaveSize / buffer_size) : 0;
        initial_units = static_cast<uint64_t>(initial_units * kEnclaveOverheadFactor);
        const uint64_t total_units =
            (initial_units > 2) ? (initial_units - 2) : 1;

        enclave_levels = 0;
        uint64_t used_units = 1;
        uint64_t gamma_pow = gamma;
        for (uint64_t i = 1; i <= max_level; ++i) {
            if (used_units + gamma_pow > total_units) break;
            used_units += gamma_pow;
            enclave_levels = i;
            gamma_pow *= gamma;
        }
    }

    void Insert(uint64_t key, uint64_t value) {
        ++next_ver_;
        write(Item(OpType::INS, key, value, next_ver_));
    }

    void Delete(uint64_t key) {
        ++next_ver_;
        write(Item(OpType::DEL, key, 0, next_ver_));
    }

    template <typename DatasetVec>
    void Ingest(DatasetVec& dataset);

    Item PointQuery(uint64_t key);
    RangeQueryResult RangeQuery(uint64_t lo, uint64_t hi);

    uint64_t level_count() const { return depth; }
    bool level_occupied(uint64_t l) const {
        return l < levels.size() && levels[l].occupied;
    }
    bool level_in_enclave(uint64_t l) const {
        return l < levels.size() && levels[l].occupied && levels[l].is_enclave;
    }

    uint64_t getByteSize() const {
        uint64_t bytes = sizeof(*this);
        
        // Include per-node overhead of std::set (typically red-black tree nodes).
        constexpr uint64_t kSetNodeOverhead = 32; 
        bytes += buffer.size() * (sizeof(Item) + kSetNodeOverhead); 
        
        // Include reserved storage of the level metadata array.
        bytes += levels.capacity() * sizeof(LevelState);
        
        // Aggregate dynamic memory owned by each level.
        for (size_t i = 0; i < levels.size(); ++i) {
            bytes += levels[i].getByteSize() - sizeof(LevelState);
        }
        
        return bytes;
    }

private:
    void write(const Item& x) {
        Item probe(OpType::INS, x.get_key(), 0, Item::kVerMax);
        auto it = buffer.lower_bound(probe);
        if (it != buffer.end() && it->get_key() == x.get_key()) {
            it = buffer.erase(it); // O(1) amortized insertion hint
        }
        buffer.insert(it, x);
        if (buffer.size() >= buffer_real_count)
            flush_buffer();
    }

    void flush_buffer();
    void place_run(RunData carry, uint64_t start_level);
    void store_at_level(uint64_t lv, RunData& run);
    void build_bloom_and_fences(LevelState& ls);
    void rebuild_ext_bptree(LevelState& ls);

    template <typename Vec1, typename Vec2>
    RunData merge_sorted(Vec1& v1, uint64_t n1,
                         Vec2& v2, uint64_t n2,
                         bool remove_del);

    template <typename InputVec>
    void bulk_load_items(InputVec& input, uint64_t input_count);
};

// =============================================================================
// Implementation
// =============================================================================

// --- flush_buffer ---

template <uint64_t CPS, uint64_t ES>
inline void KVLSMTree<CPS, ES>::flush_buffer() {
    if (buffer.empty()) return;

    const uint64_t count = buffer.size();
    RunData run;
    run.count = count;
    run.items = std::make_unique<NCVec<Item>>(count);
    {
        typename NCVec<Item>::Writer writer(run.items->begin(), run.items->end());
        for (const Item& item : buffer)
            writer.write(item);
        writer.flush();
    }
    buffer.clear();
    place_run(std::move(run), 1);
}

// --- place_run ---

template <uint64_t CPS, uint64_t ES>
inline void KVLSMTree<CPS, ES>::place_run(RunData carry, uint64_t start_level) {
    uint64_t level = start_level;

    while (level < levels.size()) {
        LevelState& dst = levels[level];

        if (!dst.occupied) {
            store_at_level(level, carry);
            return;
        }

        const bool remove_del = (level == depth);
        RunData merged;
        if (dst.is_enclave) {
            merged = merge_sorted(*carry.items, carry.count,
                                  *dst.enc_items, dst.real_count, remove_del);
        } else {
            merged = merge_sorted(*carry.items, carry.count,
                                  *dst.ext_items, dst.real_count, remove_del);
        }
        dst.clear();

        // Match SapphMap::place_run: only stay on this level if merged size is
        // strictly below capacity; at equality, cascade to the next level.
        if (merged.count < dst.ncapacity) {
            store_at_level(level, merged);
            return;
        }

        carry = std::move(merged);
        ++level;
    }
}

// --- store_at_level ---

template <uint64_t CPS, uint64_t ES>
inline void KVLSMTree<CPS, ES>::store_at_level(uint64_t lv, RunData& run) {
    LevelState& ls = levels[lv];
    ls.occupied = true;
    ls.real_count = run.count;

    if (lv <= enclave_levels) {
        ls.is_enclave = true;
        ls.enc_items = std::make_unique<StdVector<Item>>(run.count);
        typename NCVec<Item>::Reader reader(run.items->begin(),
                                            run.items->begin() + run.count);
        ls.fences.init(kItemsPerBlock);
        for (uint64_t i = 0; i < run.count; ++i) {
            Item item = reader.read();
            (*ls.enc_items)[static_cast<size_t>(i)] = item;
            ls.fences.record(i, item.get_key());
            if (i + 1 == run.count) ls.fences.set_last_key(item.get_key());
        }
        ls.ext_items.reset();
        ls.ext_blooms.reset();
        ls.ext_bptree.reset();
        ls.ext_fence_keys.reset();
    } else {
        ls.is_enclave = false;
        ls.ext_items = std::move(run.items);
        ls.enc_items.reset();
        build_bloom_and_fences(ls);
    }
    depth = std::max(depth, lv);
}

// --- build_bloom_and_fences ---

template <uint64_t CPS, uint64_t ES>
inline void KVLSMTree<CPS, ES>::build_bloom_and_fences(LevelState& ls) {
    const uint64_t num_blocks = divRoundUp(ls.real_count, kItemsPerBlock);

    ls.fences.init(kItemsPerBlock);
    ls.ext_blooms = std::make_unique<BloomVec>(num_blocks);
    typename BloomVec::Writer bloom_writer(ls.ext_blooms->begin(),
                                           ls.ext_blooms->end());

    typename NCVec<Item>::Reader reader(ls.ext_items->begin(),
                                        ls.ext_items->begin() + ls.real_count);

    uint64_t global_idx = 0;
    for (uint64_t b = 0; b < num_blocks; ++b) {
        const uint64_t block_count =
            std::min(kItemsPerBlock, ls.real_count - b * kItemsPerBlock);

        BlockBloomFilter bbf;
        bbf.clear();

        for (uint64_t i = 0; i < block_count; ++i) {
            Item item = reader.read();
            ls.fences.record(global_idx, item.get_key());
            bbf.add(item.get_key());
            if (global_idx + 1 == ls.real_count) ls.fences.set_last_key(item.get_key());
            ++global_idx;
        }
        bloom_writer.write(bbf);
    }
    bloom_writer.flush();
    rebuild_ext_bptree(ls);
}

template <uint64_t CPS, uint64_t ES>
inline void KVLSMTree<CPS, ES>::rebuild_ext_bptree(LevelState& ls) {
    ls.ext_bptree.reset();
    ls.ext_fence_keys.reset();
    if (!ls.occupied || ls.is_enclave || ls.fences.first_keys_.empty()) return;

    const uint64_t n = ls.fences.first_keys_.size();
    ls.ext_fence_keys = std::make_unique<ExtFenceKeyVec>(n);
    {
        typename ExtFenceKeyVec::Writer w(ls.ext_fence_keys->begin(),
                                          ls.ext_fence_keys->end());
        for (uint64_t i = 0; i < n; ++i)
            w.write(ls.fences.first_keys_[static_cast<size_t>(i)]);
        w.flush();
    }
    ls.ext_bptree = std::make_unique<ExtBPlusTreeType>(
        build_bplus_tree<KvBndCachedVec>(*ls.ext_fence_keys, kKvLevelBPlusTreeBlockSize,
                                        EM::Definition::DOMAIN_SIZE_D));
}

// --- merge_sorted ---

template <uint64_t CPS, uint64_t ES>
template <typename Vec1, typename Vec2>
inline typename KVLSMTree<CPS, ES>::RunData
KVLSMTree<CPS, ES>::merge_sorted(Vec1& v1, uint64_t n1,
                                   Vec2& v2, uint64_t n2,
                                   bool remove_del) {
    const uint64_t total = n1 + n2;
    auto output = std::make_unique<NCVec<Item>>(total);
    typename NCVec<Item>::Writer writer(output->begin(), output->end());

    typename Vec1::Reader reader1(v1.begin(), v1.begin() + n1);
    typename Vec2::Reader reader2(v2.begin(), v2.begin() + n2);

    Item a, b;
    uint64_t i1 = 0, i2 = 0;
    bool has_a = false, has_b = false;

    if (n1 > 0) { a = reader1.read(); i1 = 1; has_a = true; }
    if (n2 > 0) { b = reader2.read(); i2 = 1; has_b = true; }

    uint64_t written = 0;
    uint64_t prev_key = UINT64_MAX;

    while (has_a || has_b) {
        bool from_a;
        if (has_a && has_b) {
            from_a = !(b < a); 
        } else {
            from_a = has_a;
        }

        Item picked;
        if (from_a) {
            picked = a;
            if (i1 < n1) { a = reader1.read(); i1++; }
            else has_a = false;
        } else {
            picked = b;
            if (i2 < n2) { b = reader2.read(); i2++; }
            else has_b = false;
        }

        if (picked.is_dummy()) continue;
        if (picked.get_key() == prev_key) continue;
        if (remove_del && picked.get_op() == OpType::DEL) {
            prev_key = picked.get_key();
            continue;
        }

        writer.write(picked);
        written++;
        prev_key = picked.get_key();
    }

    const Item dummy;
    for (uint64_t i = written; i < total; ++i)
        writer.write(dummy);
    writer.flush();

    RunData result;
    result.items = std::move(output);
    result.count = written;
    return result;
}

// --- PointQuery ---

template <uint64_t CPS, uint64_t ES>
inline typename KVLSMTree<CPS, ES>::Item
KVLSMTree<CPS, ES>::PointQuery(uint64_t key) {
    Item target(OpType::INS, key, 0, Item::kVerMax);
    auto it = buffer.lower_bound(target);
    if (it != buffer.end() && it->get_key() == key) {
        if (it->get_op() == OpType::DEL) return Item();
        return *it;
    }

    // Scan levels from newest to oldest and stop on first exact key hit.
    for (uint64_t lv = 1; lv <= depth; ++lv) {
        const LevelState& ls = levels[lv];
        if (!ls.occupied) continue;

        if (ls.is_enclave) {
            const auto& items = *ls.enc_items;
            if (ls.fences.first_keys_.empty()) {
                int64_t lo = 0, hi = static_cast<int64_t>(ls.real_count) - 1;
                while (lo <= hi) {
                    int64_t mid = lo + (hi - lo) / 2;
                    uint64_t mk = items[static_cast<size_t>(mid)].get_key();
                    if (mk < key) lo = mid + 1;
                    else if (mk > key) hi = mid - 1;
                    else {
                        const Item& hit = items[static_cast<size_t>(mid)];
                        if (hit.get_op() == OpType::DEL) return Item();
                        return hit;
                    }
                }
            } else {
                if (ls.fences.disjoint_from_query(key, key)) continue;
                const uint64_t block_idx = ls.fences.find_page(key);
                const uint64_t offset = block_idx * kItemsPerBlock;
                const uint64_t end_off = std::min(offset + kItemsPerBlock, ls.real_count);
                if (offset >= ls.real_count) continue;
                int64_t lo = static_cast<int64_t>(offset);
                int64_t hi = static_cast<int64_t>(end_off) - 1;
                if (lo > hi) continue;
                while (lo <= hi) {
                    int64_t mid = lo + (hi - lo) / 2;
                    uint64_t mk = items[static_cast<size_t>(mid)].get_key();
                    if (mk < key) lo = mid + 1;
                    else if (mk > key) hi = mid - 1;
                    else {
                        const Item& hit = items[static_cast<size_t>(mid)];
                        if (hit.get_op() == OpType::DEL) return Item();
                        return hit;
                    }
                }
            }
        } else {
            const uint64_t block_idx = ls.ext_bptree
                ? kv_lsmtree_bptree_find_page(*ls.ext_bptree, key)
                : ls.fences.find_page(key);
            const uint64_t offset = block_idx * kItemsPerBlock;
            const uint64_t end_off = std::min(offset + kItemsPerBlock, ls.real_count);
            if (offset >= ls.real_count) continue;

            BlockBloomFilter bbf;
            {
                typename BloomVec::Reader br(
                    ls.ext_blooms->begin() + block_idx,
                    ls.ext_blooms->begin() + block_idx + 1);
                bbf = br.read();
            }
            if (!bbf.maybe_contains(key)) continue;

            // Binary search over one encrypted block using single-item readers.
            auto read_one = [&](uint64_t idx) -> Item {
                typename NCVec<Item>::Reader r(
                    ls.ext_items->begin() + idx,
                    ls.ext_items->begin() + idx + 1);
                return r.read();
            };

            int64_t lo2 = static_cast<int64_t>(offset);
            int64_t hi2 = static_cast<int64_t>(end_off) - 1;
            while (lo2 <= hi2) {
                int64_t mid2 = lo2 + (hi2 - lo2) / 2;
                uint64_t mk = read_one(static_cast<uint64_t>(mid2)).get_key();
                if (mk < key)       lo2 = mid2 + 1;
                else if (mk > key)  hi2 = mid2 - 1;
                else {
                    const Item hit = read_one(static_cast<uint64_t>(mid2));
                    if (hit.get_op() == OpType::DEL) return Item();
                    return hit;
                }
            }
        }
    }
    return Item();
}

// --- RangeQuery ---

template <uint64_t CPS, uint64_t ES>
inline auto KVLSMTree<CPS, ES>::RangeQuery(uint64_t lo, uint64_t hi)
    -> RangeQueryResult {
    struct LevelRange {
        uint64_t lv;
        uint64_t start_idx;
        uint64_t end_idx;
        bool is_enc;
    };
    std::vector<LevelRange> lranges;
    uint64_t count = 0;

    for (uint64_t lv = 1; lv <= depth; ++lv) {
        const LevelState& ls = levels[lv];
        if (!ls.occupied || ls.real_count == 0) continue;

        if (ls.is_enclave) {
            // Fence-localized binary search per block (avoids global [0, real_count) probes).
            const auto& items = *ls.enc_items;
            if (ls.fences.first_keys_.empty()) {
                int64_t left = 0, right = static_cast<int64_t>(ls.real_count);
                while (left < right) {
                    int64_t mid = left + (right - left) / 2;
                    if (items[static_cast<size_t>(mid)].get_key() < lo) left = mid + 1;
                    else right = mid;
                }
                const uint64_t first_ge_lo = static_cast<uint64_t>(left);
                if (first_ge_lo >= ls.real_count) continue;
                if (items[static_cast<size_t>(first_ge_lo)].get_key() > hi) continue;

                int64_t lo2 = 0, hi2 = static_cast<int64_t>(ls.real_count);
                while (lo2 < hi2) {
                    int64_t mid = lo2 + (hi2 - lo2) / 2;
                    if (items[static_cast<size_t>(mid)].get_key() <= hi) lo2 = mid + 1;
                    else hi2 = mid;
                }
                if (lo2 == 0) continue;
                const uint64_t last_in_range = static_cast<uint64_t>(lo2) - 1;
                if (last_in_range < first_ge_lo) continue;

                const uint64_t start_idx = (first_ge_lo / kItemsPerPage) * kItemsPerPage;
                const uint64_t end_idx =
                    std::min((last_in_range / kItemsPerPage + 1) * kItemsPerPage, ls.real_count);

                lranges.push_back({lv, start_idx, end_idx, true});
                count += (end_idx - start_idx);
            } else {
                if (ls.fences.disjoint_from_query(lo, hi)) continue;

                constexpr uint64_t kNoIdx = UINT64_MAX;
                uint64_t first_ge_lo = kNoIdx;
                const uint64_t start_block_lo = ls.fences.find_page(lo);
                for (uint64_t b = start_block_lo;; ++b) {
                    const uint64_t bs = b * kItemsPerBlock;
                    if (bs >= ls.real_count) break;
                    const uint64_t be = std::min(bs + kItemsPerBlock, ls.real_count);
                    int64_t left = static_cast<int64_t>(bs), right = static_cast<int64_t>(be);
                    while (left < right) {
                        const int64_t mid = left + (right - left) / 2;
                        if (items[static_cast<size_t>(mid)].get_key() < lo) left = mid + 1;
                        else right = mid;
                    }
                    const uint64_t cand = static_cast<uint64_t>(left);
                    if (cand < be && items[static_cast<size_t>(cand)].get_key() >= lo) {
                        first_ge_lo = cand;
                        break;
                    }
                    if (be >= ls.real_count) break;
                }
                if (first_ge_lo == kNoIdx) continue;
                if (items[static_cast<size_t>(first_ge_lo)].get_key() > hi) continue;

                uint64_t last_in_range = kNoIdx;
                for (int64_t b = static_cast<int64_t>(ls.fences.find_page(hi)); b >= 0; --b) {
                    const uint64_t bs = static_cast<uint64_t>(b) * kItemsPerBlock;
                    if (bs >= ls.real_count) continue;
                    const uint64_t be = std::min(bs + kItemsPerBlock, ls.real_count);
                    int64_t u_lo = static_cast<int64_t>(bs), u_hi = static_cast<int64_t>(be);
                    while (u_lo < u_hi) {
                        const int64_t mid = u_lo + (u_hi - u_lo) / 2;
                        if (items[static_cast<size_t>(mid)].get_key() <= hi) u_lo = mid + 1;
                        else u_hi = mid;
                    }
                    if (u_lo == static_cast<int64_t>(bs)) continue;
                    last_in_range = static_cast<uint64_t>(u_lo) - 1;
                    break;
                }
                if (last_in_range == kNoIdx) continue;
                if (last_in_range < first_ge_lo) continue;

                const uint64_t start_idx = (first_ge_lo / kItemsPerPage) * kItemsPerPage;
                const uint64_t end_idx =
                    std::min((last_in_range / kItemsPerPage + 1) * kItemsPerPage, ls.real_count);

                lranges.push_back({lv, start_idx, end_idx, true});
                count += (end_idx - start_idx);
            }
        } else {
            // External: fence-only index (no B+ tree / NCVec reads for block selection).
            if (ls.fences.first_keys_.empty()) continue;
            if (ls.fences.disjoint_from_query(lo, hi)) continue;

            const uint64_t block_start = ls.fences.find_page(lo);
            const uint64_t block_hi    = ls.fences.find_page(hi);
            if (block_hi < block_start) continue;

            const uint64_t start_idx = block_start * kItemsPerBlock;
            if (start_idx >= ls.real_count) continue;

            const uint64_t end_idx =
                std::min((block_hi + 1) * kItemsPerBlock, ls.real_count);

            auto read_key_at = [&](uint64_t idx) -> uint64_t {
                typename NCVec<Item>::Reader temp_r(
                    ls.ext_items->begin() + idx,
                    ls.ext_items->begin() + idx + 1);
                return temp_r.read().get_key();
            };

            if (block_start == block_hi) {
                const uint64_t bs = start_idx;
                const uint64_t be = end_idx;
                const uint64_t fk = ls.fences.first_keys_[static_cast<size_t>(block_start)];
                if (fk > hi) continue;
                if (fk < lo) {
                    int64_t left = static_cast<int64_t>(bs), right = static_cast<int64_t>(be);
                    while (left < right) {
                        const int64_t mid = left + (right - left) / 2;
                        if (read_key_at(static_cast<uint64_t>(mid)) < lo) left = mid + 1;
                        else right = mid;
                    }
                    const uint64_t first_ge = static_cast<uint64_t>(left);
                    if (first_ge >= be) continue;
                    if (read_key_at(first_ge) > hi) continue;
                }
            }

            lranges.push_back({lv, start_idx, end_idx, false});
            count += (end_idx - start_idx);
        }
    }

    const Item target_lo(OpType::INS, lo, 0, Item::kVerMax);
    uint64_t buf_cnt = 0;
    for (auto it = buffer.lower_bound(target_lo); it != buffer.end() && it->get_key() <= hi; ++it) {
        if (it->get_key() < lo) continue;
        ++buf_cnt;
    }
    count += buf_cnt;

    auto fill_output = [&](auto& output) {
        using OutVec = std::decay_t<decltype(output)>;
        typename OutVec::Writer writer(output.begin(), output.end());

        for (auto it = buffer.lower_bound(target_lo); it != buffer.end() && it->get_key() <= hi; ++it) {
            if (it->get_key() < lo) continue;
            writer.write(*it);
        }

        for (const LevelRange& lr : lranges) {
            const LevelState& ls = levels[lr.lv];
            if (lr.is_enc) {
                const auto& items = *ls.enc_items;
                for (uint64_t idx = lr.start_idx; idx < lr.end_idx; ++idx)
                    writer.write(items[static_cast<size_t>(idx)]);
            } else {
                typename NCVec<Item>::Reader reader(ls.ext_items->begin() + lr.start_idx,
                                                    ls.ext_items->begin() + lr.end_idx);
                const uint64_t n = lr.end_idx - lr.start_idx;
                for (uint64_t k = 0; k < n; ++k)
                    writer.write(reader.read());
            }
        }
        writer.flush();
    };

    if (count == 0) {
        if (enclave_levels >= depth) return RangeQueryResult(StdVec<Item>{});
        return RangeQueryResult(std::move(NCVec<Item>(0)));
    }
    if (enclave_levels >= depth) {
        StdVec<Item> output(static_cast<uint64_t>(count));
        fill_output(output);
        return RangeQueryResult(std::move(output));
    }
    NCVec<Item> output(count);
    fill_output(output);
    return RangeQueryResult(std::move(output));
}

// --- Ingest ---

template <uint64_t CPS, uint64_t ES>
template <typename DatasetVec>
inline void KVLSMTree<CPS, ES>::Ingest(DatasetVec& dataset) {
    if (dataset.size() == 0) return;

    ++next_ver_;
    const uint64_t ver = next_ver_;
    const uint64_t n_input = dataset.size();

    const uint64_t sort_heap = ES / 2;
    EM::Primitive::ExtMergeSort<false>(dataset, sort_heap);

    uint64_t unique_count = 0;
    {
        typename DatasetVec::Reader reader(dataset.begin(), dataset.end());
        uint64_t prev_key = UINT64_MAX;
        for (uint64_t i = 0; i < n_input; ++i) {
            KvPair kv = reader.read();
            if (kv.key != prev_key) {
                unique_count++;
                prev_key = kv.key;
            }
        }
    }
    if (unique_count == 0) return;

    NCVec<Item> items_vec(unique_count);
    {
        typename DatasetVec::Reader reader(dataset.begin(), dataset.end());
        typename NCVec<Item>::Writer writer(items_vec.begin(), items_vec.end());
        uint64_t prev_key = UINT64_MAX;
        for (uint64_t i = 0; i < n_input; ++i) {
            KvPair kv = reader.read();
            if (kv.key != prev_key) {
                writer.write(Item(OpType::INS, kv.key, kv.value, ver));
                prev_key = kv.key;
            }
        }
        writer.flush();
    }

    bulk_load_items(items_vec, unique_count);
}

// --- bulk_load_items ---

template <uint64_t CPS, uint64_t ES>
template <typename InputVec>
inline void KVLSMTree<CPS, ES>::bulk_load_items(InputVec& input, uint64_t input_count) {
    const uint64_t buf_size = buffer.size();
    const uint64_t base_total = buf_size + input_count;

    if (base_total < buffer_real_count) {
        typename InputVec::Reader reader(input.begin(), input.begin() + input_count);
        for (uint64_t i = 0; i < input_count; ++i) {
            Item x = reader.read();
            Item probe(OpType::INS, x.get_key(), 0, Item::kVerMax);
            auto it = buffer.lower_bound(probe);
            if (it != buffer.end() && it->get_key() == x.get_key()) {
                it = buffer.erase(it);
            }
            buffer.insert(it, x);
        }
        return;
    }

    uint64_t accumulated = base_total;
    uint64_t target = 1;
    while (target < levels.size() && accumulated > levels[target].ncapacity) {
        if (levels[target].occupied)
            accumulated += levels[target].real_count;
        ++target;
    }
    if (target >= levels.size()) target = levels.size() - 1;

    RunData carry;
    if (buf_size > 0) {
        StdVector<Item> std_buf(buf_size);
        uint64_t idx = 0;
        for (const Item& item : buffer)
            std_buf[static_cast<size_t>(idx++)] = item;
        buffer.clear();

        carry = merge_sorted(std_buf, buf_size, input, input_count, false);
    } else {
        buffer.clear();
        carry.items = std::make_unique<NCVec<Item>>(input_count);
        typename InputVec::Reader reader(input.begin(), input.begin() + input_count);
        typename NCVec<Item>::Writer writer(carry.items->begin(), carry.items->end());
        for (uint64_t i = 0; i < input_count; ++i)
            writer.write(reader.read());
        writer.flush();
        carry.count = input_count;
    }

    for (uint64_t j = 1; j < target; ++j) {
        if (!levels[j].occupied) continue;
        RunData merged;
        if (levels[j].is_enclave) {
            merged = merge_sorted(*carry.items, carry.count,
                                  *levels[j].enc_items, levels[j].real_count, false);
        } else {
            merged = merge_sorted(*carry.items, carry.count,
                                  *levels[j].ext_items, levels[j].real_count, false);
        }
        levels[j].clear();
        carry = std::move(merged);
    }
    place_run(std::move(carry), target);
}

}  // namespace EM::Sapphmap