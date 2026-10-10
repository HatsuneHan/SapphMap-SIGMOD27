#pragma once

/**
 * @file no_hashmap.hpp
 * @brief Batched-flush hash map baseline for external-memory experiments.
 *
 * NoHashMap is a non-oblivious baseline designed for empirical comparison
 * with SapphMap.  It uses a two-level architecture:
 *
 *   L1 (enclave):  16-byte key/value entries with open addressing and a
 *                   packed 2-bit state array.
 *                   Absorbs all writes.  Flushed batch-by-bucket to L2.
 *   L2 (external): Single NCVec of dense 16-byte key/value entries divided
 *                   into equal-sized buckets.
 *                   Updated via batched merge during flush.
 *
 * Flush I/O is batched and page-aligned. Point lookups read one encrypted
 * external-memory page selected by the in-enclave metadata.
 */

#include <algorithm>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

#include "common/defs.hpp"
#include "common/utils.hpp"
#include "external_memory/noncachedvector.hpp"
#include "external_memory/primitive/sorting/sort_def.hpp"
#include "external_memory/sapphmap/pds_def.hpp"
#include "external_memory/stdvector.hpp"

// NOTE: We intentionally do NOT include sapphmap.hpp here to avoid triggering
// a condSwap template-instantiation-order issue in or_compact_shuffle.hpp.
// The caller (e.g. sapphmap_test.cpp) must include experiment.hpp (which pulls
// in sapphmap.hpp) BEFORE including this header.

namespace EM::Sapphmap {

// ============================================================================
// Hash function: SplitMix64
// ============================================================================

static inline uint64_t no_hashmap_mix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x = x ^ (x >> 31);
    return x;
}

// ============================================================================
// Bloom filter constants
// ============================================================================

static constexpr uint64_t kBFDefaultNumHashes = 7;

// ============================================================================
// NoHashMap
// ============================================================================

template <uint64_t CachePageSize = BLOCK_SIZE, uint64_t EnclaveSize = ENCLAVE_SIZE>
struct NoHashMap {
    // Item remains the public benchmark result type.  It is never stored in
    // either tier: the hash map itself stores only an 8-byte key and 8-byte
    // value.  Versions and per-record operation/dummy fields are unnecessary
    // because L1 holds at most one newest record per key and always wins over
    // L2 during lookup/merge.
    using Item = EM::Definition::Item;
    using OpType = EM::Definition::OpType;

    struct Entry {
        uint64_t key = 0;
        uint64_t value = 0;
    };
    static_assert(sizeof(Entry) == 16, "NoHashMap Entry must be 16 bytes");
    static_assert(std::is_trivially_copyable_v<Entry>,
                  "NoHashMap Entry must be trivially copyable");

    enum class SlotState : uint8_t {
        Empty = 0,
        Live = 1,
        Deleted = 2,
    };

    struct PendingEntry {
        Entry kv;
        SlotState state = SlotState::Empty;
    };

    // Transient record used only by the offline bulk builder.  sequence keeps
    // last-write-wins semantics when an ingest input contains duplicate keys.
    // It is never stored in either persistent tier.
    struct BulkEntry {
        Entry kv;
        uint64_t sequence = 0;
    };

    // NCVec: same page layout as KVLSMTree
    template <typename T, auto...>
    using NCVec = EM::NonCachedVector::Vector<T, CachePageSize - 32, true, true>;

    // L2 is capacity-sized from max_writes, but only a prefix of each bucket is
    // populated.  Keep untouched pages as authenticated logical dummies behind
    // the frontend's trusted modified bitmap instead of encrypting/writing the
    // entire capacity during the first flush.  The public NCVec alias remains
    // eagerly initialized because the benchmark driver also uses it for input
    // replay batches.
    using L2Vec = EM::NonCachedVector::Vector<
        Entry, CachePageSize - 32, true, true, true>;

    // StdVec: enclave-resident vector (inherits std::vector)
    template <typename T>
    using StdVec = StdVector<T>;

    // RangeQueryResult: dual-representation (enclave StdVec or external NCVec).
    struct RangeQueryResult {
        bool std_active = true;
        std::unique_ptr<StdVec<Item>> std_vec;
        std::unique_ptr<NCVec<Item>> nc_vec;

        RangeQueryResult(StdVec<Item>&& v)
            : std_active(true), std_vec(std::make_unique<StdVec<Item>>(std::move(v))) {}
        RangeQueryResult(NCVec<Item>&& v)
            : std_active(false), nc_vec(std::make_unique<NCVec<Item>>(std::move(v))) {}

        bool is_stdvec() const { return std_active; }

        uint64_t size() const {
            return std_active
                ? static_cast<uint64_t>(std_vec ? std_vec->size() : 0)
                : static_cast<uint64_t>(nc_vec ? nc_vec->size() : 0);
        }

        StdVec<Item>& as_stdvec() { return *std_vec; }
        const StdVec<Item>& as_stdvec() const { return *std_vec; }
        NCVec<Item>& as_ncvec() { return *nc_vec; }
        const NCVec<Item>& as_ncvec() const { return *nc_vec; }
    };

    // Use NCVec's physical page layout.  Recomputing this from CachePageSize is
    // incorrect because NonCachedVector rounds its item count to a power of two.
    static constexpr uint64_t kItemsPerPage = NCVec<Entry>::item_per_page;
    static_assert(kItemsPerPage > 0, "CachePageSize too small for Entry");

    static constexpr uint64_t kL1StateBits = 2;
    static constexpr uint64_t kL1StatesPerWord = 64 / kL1StateBits;
    // Budget one byte per state even though the actual packed representation
    // uses only two bits.  This leaves allocator/alignment headroom.
    static constexpr uint64_t kL1BudgetBytesPerSlot = sizeof(Entry) + 1;

    // Flush threshold: flush when L1 reaches this fraction of capacity
    static constexpr uint64_t kFlushNumerator   = 8;
    static constexpr uint64_t kFlushDenominator = 10;

    // L1 starts small and grows geometrically.  This is deliberately
    // independent of max_writes: max_writes describes the fixed L2/backend
    // upper bound, not the amount of enclave memory to reserve up front.
    static constexpr uint64_t kInitialL1Capacity = (1ULL << 16);
    static constexpr uint64_t kMinimumL1Capacity = 16;

    // Compute a flush/scratch-safe bound before applying the stricter
    // allocator-fragmentation bound below.  The flush threshold bounds
    // flush_sorted_ by 0.8 * capacity.
    static constexpr uint64_t kL1SafetyNumerator   = 9;
    static constexpr uint64_t kL1SafetyDenominator = 10;

    // SGX's enclave allocator may retain or fragment blocks from earlier
    // geometric rehashes.  Limit the persistent L1 table to one quarter of the
    // post-metadata slot budget.  Even if all earlier table allocations remain
    // unusable, a final rehash plus the largest flush buffer stays well below
    // the heap limit and the map flushes to L2 before a giant final rehash.
    static constexpr uint64_t kL1AllocatorBudgetDenominator = 4;

    static bool checked_add_u64(uint64_t a, uint64_t b, uint64_t& out) {
        if (a > UINT64_MAX - b) return false;
        out = a + b;
        return true;
    }

    static bool checked_mul_u64(uint64_t a, uint64_t b, uint64_t& out) {
        if (a != 0 && b > UINT64_MAX / a) return false;
        out = a * b;
        return true;
    }

    static uint64_t l1_state_word_count(uint64_t capacity) {
        return capacity / kL1StatesPerWord +
            ((capacity % kL1StatesPerWord) != 0 ? 1ULL : 0ULL);
    }

    // Skew factor for L2 bucket capacity
    static constexpr uint64_t kSkewExtraDenominator = 2;

    // Preserve the previous fixed 8 MiB reserve for stack/temporaries.  It was
    // formerly expressed as 2^18 32-byte Item slots.
    static constexpr uint64_t kReservedBytes = (8ULL << 20);

    // Backend construction/replay headroom.  The L2 allocation is long-lived,
    // while benchmark input batches temporarily consume additional backend
    // pages.  Keep proportional headroom plus enough room for the 2^20-op
    // replay batch used by experiment.hpp.
    static constexpr uint64_t kBackendHeadroomNumerator = 1;
    static constexpr uint64_t kBackendHeadroomDenominator = 4;
    static constexpr uint64_t kBackendFixedHeadroom = (128ULL << 20);

    // Exp1 may time Ingest as a genuine offline build.  Ingest has its own
    // pre-sized-L1/direct-L2 implementation and does not call the sequential
    // Insert/flush path used by the subsequent seq-build and PQ phases.
    static constexpr bool kSupportsExp1BulkBuild = true;

    static uint64_t ComputeBackendBytes(uint64_t max_writes) {
        uint64_t nb = 1;
        while (nb < max_writes && nb <= max_writes / nb) {
            nb <<= 1;
        }

        const uint64_t expected = max_writes / nb +
            ((max_writes % nb) != 0 ? 1ULL : 0ULL);
        const uint64_t skew_extra =
            expected / kSkewExtraDenominator +
            ((expected % kSkewExtraDenominator) != 0 ? 1ULL : 0ULL);
        if (expected > UINT64_MAX - skew_extra) {
            return UINT64_MAX;
        }
        const uint64_t skewed = expected + skew_extra;
        if (skewed > UINT64_MAX - (kItemsPerPage - 1)) {
            return UINT64_MAX;
        }
        const uint64_t aligned_units =
            (skewed + kItemsPerPage - 1) / kItemsPerPage;
        if (aligned_units > UINT64_MAX / kItemsPerPage) {
            return UINT64_MAX;
        }
        uint64_t items_per_bucket = aligned_units * kItemsPerPage;
        if (items_per_bucket < kItemsPerPage) {
            items_per_bucket = kItemsPerPage;
        }

        const uint64_t pages_per_bucket = items_per_bucket / kItemsPerPage;
        if (nb > UINT64_MAX / pages_per_bucket) {
            return UINT64_MAX;
        }
        const uint64_t pages = nb * pages_per_bucket;
        using Page = typename NCVec<Entry>::Page;
        using EncPage = typename Page::Encrypted_t;
        if (pages > UINT64_MAX / sizeof(EncPage)) {
            return UINT64_MAX;
        }
        return pages * sizeof(EncPage);
    }

    static uint64_t ComputeDatasetBackendBytes(uint64_t item_count) {
        using DatasetVec = NCVec<Entry>;
        using Page = typename DatasetVec::Page;
        using EncPage = typename Page::Encrypted_t;
        const uint64_t items_per_page = DatasetVec::item_per_page;
        const uint64_t pages = item_count / items_per_page +
            ((item_count % items_per_page) != 0 ? 1ULL : 0ULL);
        if (pages > UINT64_MAX / sizeof(EncPage)) return UINT64_MAX;
        return pages * sizeof(EncPage);
    }

    static uint64_t RecommendBackendSize(uint64_t init_size,
                                         uint64_t max_writes,
                                         bool is_exp1_split) {
        const uint64_t logical_writes =
            (init_size > max_writes) ? init_size : max_writes;
        const uint64_t base = ComputeBackendBytes(logical_writes);
        if (base == UINT64_MAX) return UINT64_MAX;
        const uint64_t proportional =
            base / kBackendHeadroomDenominator * kBackendHeadroomNumerator;
        const uint64_t dataset = is_exp1_split
            ? ComputeDatasetBackendBytes(init_size)
            : 0;
        if (dataset == UINT64_MAX) return UINT64_MAX;
        uint64_t total = 0;
        if (!checked_add_u64(base, proportional, total) ||
            !checked_add_u64(total, kBackendFixedHeadroom, total) ||
            !checked_add_u64(total, dataset, total)) {
            return UINT64_MAX;
        }
        return total;
    }

    // ========================================================================
    // Public members (required by benchmark driver)
    // ========================================================================
    uint64_t enclave_levels;
    uint64_t buffer_real_count;

    // ========================================================================
    // State
    // ========================================================================
    uint64_t l1_capacity_;
    uint64_t l1_capacity_limit_;
    uint64_t l1_slot_budget_;
    uint64_t l1_count_;
    uint64_t l1_deleted_count_;
    uint64_t l1_peak_capacity_;
    uint64_t l1_rehash_count_;
    uint64_t l1_flush_count_;
    uint64_t num_buckets_;
    uint64_t items_per_bucket_;

    // L1: enclave hash table (open addressing + linear probing)
    StdVec<Entry> l1_table_;
    // Two bits per L1 slot: Empty, Live, or Deleted.  Keeping state out of the
    // entry preserves a true 16-byte key/value record and supports every
    // uint64_t key without reserving sentinel values.
    StdVec<uint64_t> l1_states_;

    // L2: external data buckets (single contiguous NCVec)
    // Allocated lazily on first use (after g_DefaultBackend is set).
    std::unique_ptr<L2Vec> l2_data_ptr_;

    // Bridge metadata: enclave-resident
    StdVec<uint64_t> bucket_sizes_;
    // Authentication epoch for every L2 bucket.  All pages in a bucket are
    // rewritten together, so one counter per bucket prevents GCM nonce reuse.
    StdVec<uint32_t> bucket_auth_versions_;

    // Dynamic Bloom filter: flat array of uint64_t words
    uint64_t bf_bits_per_bucket_;
    uint64_t bf_words_per_bucket_;
    uint64_t bf_num_hashes_;
    StdVec<uint64_t> bf_data_;

    // Page-level index for binary search in L2 buckets (enclave-resident)
    // Flat array: num_buckets_ × pages_per_bucket_ entries, storing first key per page.
    uint64_t pages_per_bucket_;
    StdVec<uint64_t> page_index_;

    // Reusable flush buffers (avoid per-flush malloc/free in enclave)
    std::vector<uint64_t> flush_counts_;
    std::vector<uint64_t> flush_fill_;
    std::vector<PendingEntry> flush_sorted_;

    // ========================================================================
    // Constructor
    // ========================================================================

    NoHashMap(uint64_t buffer_size, uint64_t gamma, uint64_t max_writes)
        : enclave_levels(1)
        , buffer_real_count(0)
        , l1_capacity_(0)
        , l1_capacity_limit_(0)
        , l1_slot_budget_(0)
        , l1_count_(0)
        , l1_deleted_count_(0)
        , l1_peak_capacity_(0)
        , l1_rehash_count_(0)
        , l1_flush_count_(0)
        , num_buckets_(0)
        , items_per_bucket_(0)
        , bf_bits_per_bucket_(0)
        , bf_words_per_bucket_(0)
        , bf_num_hashes_(kBFDefaultNumHashes)
        , pages_per_bucket_(0)
    {
        (void)buffer_size;
        (void)gamma;

        // --- Compute L2 bucket count ---
        uint64_t nb = 1;
        {
            uint64_t target = 1;
            while (target < max_writes && target <= max_writes / target) {
                target <<= 1;
            }
            nb = (target < 1) ? 1 : target;
        }
        num_buckets_ = nb;

        // --- Compute L2 items per bucket (with skew factor, page-aligned) ---
        {
            uint64_t expected = max_writes / nb +
                ((max_writes % nb) != 0 ? 1ULL : 0ULL);
            // ceil(1.5 * expected), expressed with integers to avoid precision
            // loss for large capacities.
            const uint64_t skew_extra =
                expected / kSkewExtraDenominator +
                ((expected % kSkewExtraDenominator) != 0 ? 1ULL : 0ULL);
            if (expected > UINT64_MAX - skew_extra) {
                printf("[FATAL] NoHashMap: L2 bucket size overflow\n");
                X_FAIL();
            }
            const uint64_t skewed = expected + skew_extra;
            if (skewed > UINT64_MAX - (kItemsPerPage - 1)) {
                printf("[FATAL] NoHashMap: L2 page alignment overflow\n");
                X_FAIL();
            }
            const uint64_t aligned_units =
                (skewed + kItemsPerPage - 1) / kItemsPerPage;
            if (aligned_units > UINT64_MAX / kItemsPerPage) {
                printf("[FATAL] NoHashMap: L2 bucket capacity overflow\n");
                X_FAIL();
            }
            items_per_bucket_ = aligned_units * kItemsPerPage;
            if (items_per_bucket_ < kItemsPerPage) items_per_bucket_ = kItemsPerPage;
        }

        // --- Compute pages per bucket for page-level index ---
        pages_per_bucket_ = items_per_bucket_ / kItemsPerPage;
        if (pages_per_bucket_ < 1) pages_per_bucket_ = 1;

        // --- Adaptive Bloom filter sizing ---
        {
            uint64_t bf_budget_bits = (EnclaveSize / 10ULL) * 8ULL;
            const uint64_t desired_bits_per_bucket =
                (items_per_bucket_ > UINT64_MAX / 8ULL)
                    ? UINT64_MAX
                    : items_per_bucket_ * 8ULL;
            const uint64_t budget_bits_per_bucket =
                (nb > 0) ? bf_budget_bits / nb : 64;
            bf_bits_per_bucket_ =
                std::min(desired_bits_per_bucket, budget_bits_per_bucket);
            if (bf_bits_per_bucket_ < 64) bf_bits_per_bucket_ = 64;
            bf_words_per_bucket_ = (bf_bits_per_bucket_ + 63) / 64;
            bf_bits_per_bucket_ = bf_words_per_bucket_ * 64;

            double bits_per_item = (items_per_bucket_ > 0)
                ? static_cast<double>(bf_bits_per_bucket_) / static_cast<double>(items_per_bucket_)
                : 1.0;
            if (bits_per_item > 8.0) bits_per_item = 8.0;

            bf_num_hashes_ = static_cast<uint64_t>(0.6931 * bits_per_item);
            if (bf_num_hashes_ < 1) bf_num_hashes_ = 1;
        }

        // --- Compute the L1 EPC budget (uses metadata and flush scratch) ---
        {
            if (EnclaveSize <= kReservedBytes) {
                printf("[FATAL] NoHashMap: EnclaveSize is too small for "
                       "reserved scratch space\n");
                X_FAIL();
            }

            const uint64_t remaining_bytes = EnclaveSize - kReservedBytes;
            // Flush bookkeeping and per-bucket merge scratch are kept in
            // enclave memory as well. Account for their eventual peak here so
            // the L1 limit is not derived only from the persistent metadata.
            uint64_t meta_bytes = 0;
            auto add_meta_bytes = [&](uint64_t count, uint64_t width) {
                uint64_t term = 0;
                if (!checked_mul_u64(count, width, term) ||
                    !checked_add_u64(meta_bytes, term, meta_bytes)) {
                    meta_bytes = UINT64_MAX;
                    return false;
                }
                return true;
            };
            uint64_t bf_width = 0, page_width = 0;
            uint64_t flush_slots = 0, l2_scratch_slots = 0;
            uint64_t l2_page_count = 0;
            const bool widths_ok =
                checked_mul_u64(bf_words_per_bucket_, sizeof(uint64_t), bf_width) &&
                checked_mul_u64(pages_per_bucket_, sizeof(uint64_t), page_width) &&
                checked_mul_u64(3, nb, flush_slots) &&
                checked_mul_u64(2, items_per_bucket_, l2_scratch_slots) &&
                checked_mul_u64(nb, pages_per_bucket_, l2_page_count);
            bool meta_ok = widths_ok &&
                add_meta_bytes(nb, bf_width) &&
                add_meta_bytes(nb, sizeof(uint64_t)) &&
                add_meta_bytes(nb, sizeof(uint32_t)) &&
                add_meta_bytes(nb, page_width) &&
                add_meta_bytes(flush_slots, sizeof(uint64_t)) &&
                add_meta_bytes(l2_scratch_slots, sizeof(Entry));
            if (meta_ok) {
                const uint64_t late_init_bitmap_bytes =
                    l2_page_count / 8ULL +
                    ((l2_page_count % 8ULL) != 0 ? 1ULL : 0ULL);
                meta_ok = checked_add_u64(meta_bytes,
                                          late_init_bitmap_bytes,
                                          meta_bytes);
            }
            if (!meta_ok) {
                printf("[FATAL] NoHashMap: enclave metadata size overflow\n");
                X_FAIL();
            }
            if (meta_bytes >= remaining_bytes) {
                printf("[FATAL] NoHashMap: L2 metadata consumes the "
                       "entire enclave budget\n");
                X_FAIL();
            }

            const uint64_t l1_budget_bytes = remaining_bytes - meta_bytes;
            l1_slot_budget_ = l1_budget_bytes / kL1BudgetBytesPerSlot;
            // Reserve a safety margin first, then reserve space for a
            // worst-case PendingEntry flush buffer (up to 0.8 * capacity).
            const uint64_t safe_bytes =
                (l1_budget_bytes / kL1SafetyDenominator) *
                kL1SafetyNumerator;
            const uint64_t flush_denominator =
                kL1BudgetBytesPerSlot * kFlushDenominator +
                sizeof(PendingEntry) * kFlushNumerator;
            const uint64_t flush_safe_limit =
                (safe_bytes / flush_denominator) * kFlushDenominator +
                ((safe_bytes % flush_denominator) * kFlushDenominator) /
                    flush_denominator;
            const uint64_t allocator_safe_limit =
                l1_slot_budget_ / kL1AllocatorBudgetDenominator;
            l1_capacity_limit_ =
                std::min(flush_safe_limit, allocator_safe_limit);
            if (l1_capacity_limit_ < kMinimumL1Capacity) {
                printf("[FATAL] NoHashMap: insufficient EPC budget for "
                       "the minimum L1 table\n");
                X_FAIL();
            }

            uint64_t initial_capacity = std::min(
                kInitialL1Capacity, l1_capacity_limit_);
            // Keep normal capacities powers of two.  If a tiny test enclave
            // cannot hold 2^16 slots, use the largest power of two that fits.
            uint64_t rounded_capacity = 1;
            while (rounded_capacity <= initial_capacity / 2) {
                rounded_capacity <<= 1;
            }
            if (rounded_capacity < kMinimumL1Capacity) {
                printf("[FATAL] NoHashMap: initial L1 capacity is too "
                       "small\n");
                X_FAIL();
            }
            l1_capacity_ = rounded_capacity;
            l1_peak_capacity_ = l1_capacity_;
        }
        buffer_real_count = l1_capacity_;

        // --- Allocate L1 and metadata (StdVector, no backend needed) ---
        l1_table_.resize(l1_capacity_);
        l1_states_.resize(l1_state_word_count(l1_capacity_));
        std::fill(l1_states_.begin(), l1_states_.end(), uint64_t(0));

        bucket_sizes_.resize(num_buckets_);
        bucket_auth_versions_.resize(num_buckets_);

        uint64_t bf_slots = 0;
        uint64_t page_slots = 0;
        if (!checked_mul_u64(num_buckets_, bf_words_per_bucket_, bf_slots) ||
            !checked_mul_u64(num_buckets_, pages_per_bucket_, page_slots)) {
            printf("[FATAL] NoHashMap: metadata element count overflow\n");
            X_FAIL();
        }
        bf_data_.resize(bf_slots);
        for (size_t i = 0; i < bf_data_.size(); ++i) bf_data_[i] = 0;

        page_index_.resize(page_slots);
        for (size_t i = 0; i < page_index_.size(); ++i) page_index_[i] = UINT64_MAX;

        // L2 (NCVec) is allocated lazily — requires g_DefaultBackend.
        l2_data_ptr_ = nullptr;
    }

    // Reserve the capacity-sized L2 address range.  L2Vec's late-init frontend
    // leaves physical pages untouched until a bucket prefix is first written.
    void ensure_l2() {
        if (!l2_data_ptr_) {
            uint64_t total_l2_slots = 0;
            if (!checked_mul_u64(num_buckets_, items_per_bucket_, total_l2_slots)) {
                printf("[FATAL] NoHashMap: L2 slot count overflow\n");
                X_FAIL();
                return;
            }
            l2_data_ptr_ = std::make_unique<L2Vec>(total_l2_slots, Entry{});
        }
    }

    // ========================================================================
    // Hash function
    // ========================================================================

    static uint64_t hash(uint64_t x) {
        return no_hashmap_mix(x);
    }

    // ========================================================================
    // Dynamic Bloom filter helpers
    // ========================================================================

    void bf_clear(uint64_t b) {
        uint64_t base = b * bf_words_per_bucket_;
        for (uint64_t i = 0; i < bf_words_per_bucket_; ++i) {
            bf_data_[static_cast<size_t>(base + i)] = 0;
        }
    }

    void bf_add(uint64_t b, uint64_t key) {
        uint64_t h1 = no_hashmap_mix(key);
        uint64_t h2 = no_hashmap_mix(key ^ 0x9E3779B97F4A7C15ULL);
        uint64_t base = b * bf_words_per_bucket_;
        for (uint64_t i = 0; i < bf_num_hashes_; ++i) {
            uint64_t pos = (h1 + i * h2) % bf_bits_per_bucket_;
            bf_data_[static_cast<size_t>(base + pos / 64)] |= (1ULL << (pos % 64));
        }
    }

    bool bf_maybe_contains(uint64_t b, uint64_t key) const {
        uint64_t h1 = no_hashmap_mix(key);
        uint64_t h2 = no_hashmap_mix(key ^ 0x9E3779B97F4A7C15ULL);
        uint64_t base = b * bf_words_per_bucket_;
        for (uint64_t i = 0; i < bf_num_hashes_; ++i) {
            uint64_t pos = (h1 + i * h2) % bf_bits_per_bucket_;
            if (!(bf_data_[static_cast<size_t>(base + pos / 64)] & (1ULL << (pos % 64))))
                return false;
        }
        return true;
    }

    // ========================================================================
    // L1 helpers
    // ========================================================================

    static uint64_t l1_load_limit(uint64_t capacity) {
        return capacity - capacity / 5;  // floor(0.8 * capacity)
    }

    SlotState l1_state(uint64_t slot) const {
        const uint64_t word = slot / kL1StatesPerWord;
        const uint64_t shift = (slot % kL1StatesPerWord) * kL1StateBits;
        return static_cast<SlotState>(
            (l1_states_[static_cast<size_t>(word)] >> shift) & 0x3ULL);
    }

    static SlotState packed_state(const StdVec<uint64_t>& states,
                                  uint64_t slot) {
        const uint64_t word = slot / kL1StatesPerWord;
        const uint64_t shift = (slot % kL1StatesPerWord) * kL1StateBits;
        return static_cast<SlotState>(
            (states[static_cast<size_t>(word)] >> shift) & 0x3ULL);
    }

    static void set_packed_state(StdVec<uint64_t>& states, uint64_t slot,
                                 SlotState state) {
        const uint64_t word = slot / kL1StatesPerWord;
        const uint64_t shift = (slot % kL1StatesPerWord) * kL1StateBits;
        const uint64_t mask = 0x3ULL << shift;
        uint64_t& bits = states[static_cast<size_t>(word)];
        bits = (bits & ~mask) |
            (static_cast<uint64_t>(state) << shift);
    }

    void set_l1_state(uint64_t slot, SlotState state) {
        set_packed_state(l1_states_, slot, state);
    }

    void assign_l1_slot(uint64_t slot, const Entry& entry, SlotState state) {
        const SlotState old = l1_state(slot);
        if (old == SlotState::Empty) {
            ++l1_count_;
        } else if (old == SlotState::Deleted) {
            --l1_deleted_count_;
        }
        if (state == SlotState::Deleted) {
            ++l1_deleted_count_;
        }
        l1_table_[static_cast<size_t>(slot)] = entry;
        set_l1_state(slot, state);
    }

    // Return the next capacity.  The final growth step may be smaller than a
    // factor of two when the EPC-derived limit is not a power of two.
    uint64_t next_l1_capacity() const {
        if (l1_capacity_ >= l1_capacity_limit_) return l1_capacity_;
        if (l1_capacity_ > l1_capacity_limit_ / 2) {
            return l1_capacity_limit_;
        }
        return l1_capacity_ * 2;
    }

    bool can_rehash_to(uint64_t new_capacity) const {
        if (new_capacity <= l1_capacity_ ||
            new_capacity > l1_capacity_limit_) {
            return false;
        }
        // During rehash both vectors are live.  l1_slot_budget_ is already
        // after metadata/reserve subtraction; the safety margin and flush
        // scratch reservation are represented by l1_capacity_limit_.
        if (new_capacity > l1_slot_budget_ ||
            l1_table_.capacity() > l1_slot_budget_ - new_capacity) {
            return false;
        }
        return true;
    }

    void rehash_l1(uint64_t new_capacity) {
        if (!can_rehash_to(new_capacity)) return;

        StdVec<Entry> new_table;
        new_table.resize(new_capacity);
        StdVec<uint64_t> new_states(
            l1_state_word_count(new_capacity), uint64_t(0));
        uint64_t new_count = 0;
        uint64_t new_deleted_count = 0;

        for (uint64_t i = 0; i < l1_capacity_; ++i) {
            const SlotState state = l1_state(i);
            if (state == SlotState::Empty) continue;
            const Entry& entry = l1_table_[static_cast<size_t>(i)];

            const uint64_t idx = hash(entry.key) % new_capacity;
            bool placed = false;
            for (uint64_t probe = 0; probe < new_capacity; ++probe) {
                const uint64_t slot = (idx + probe) % new_capacity;
                if (packed_state(new_states, slot) == SlotState::Empty) {
                    new_table[static_cast<size_t>(slot)] = entry;
                    set_packed_state(new_states, slot, state);
                    ++new_count;
                    if (state == SlotState::Deleted) ++new_deleted_count;
                    placed = true;
                    break;
                }
            }
            if (!placed) {
                // This should be unreachable because growth is requested
                // before the 0.8 load threshold.  Treat it as a hard failure
                // rather than silently dropping a pending write/delete.
                printf("[FATAL] NoHashMap: L1 rehash could not place "
                       "an item (capacity=%llu)\n",
                       (unsigned long long)new_capacity);
                X_FAIL();
                return;
            }
        }

        l1_table_.swap(new_table);
        l1_states_.swap(new_states);
        l1_capacity_ = new_capacity;
        l1_count_ = new_count;
        l1_deleted_count_ = new_deleted_count;
        if (l1_capacity_ > l1_peak_capacity_) {
            l1_peak_capacity_ = l1_capacity_;
        }
        buffer_real_count = l1_capacity_;
        ++l1_rehash_count_;
    }

    bool grow_l1_for(uint64_t required_count) {
        if (required_count <= l1_load_limit(l1_capacity_)) return true;
        if (l1_capacity_ >= l1_capacity_limit_) return false;

        const uint64_t new_capacity = next_l1_capacity();
        if (new_capacity == l1_capacity_ ||
            required_count > l1_load_limit(new_capacity) ||
            !can_rehash_to(new_capacity)) {
            return false;
        }
        rehash_l1(new_capacity);
        return true;
    }

    void l1_insert(const Entry& entry, SlotState state) {
        const uint64_t key = entry.key;
        uint64_t idx = hash(key) % l1_capacity_;

        for (uint64_t i = 0; i < l1_capacity_; ++i) {
            const uint64_t slot = (idx + i) % l1_capacity_;
            const SlotState existing_state = l1_state(slot);
            if (existing_state == SlotState::Empty) break;

            // DEL is a pending logical delete, not an open-addressing tombstone.
            // It must remain discoverable until it is merged into L2.
            if (l1_table_[static_cast<size_t>(slot)].key == key) {
                assign_l1_slot(slot, entry, state);
                return;
            }
        }

        if (l1_count_ == UINT64_MAX ||
            !grow_l1_for(l1_count_ + 1)) {
            // If the table cannot grow without violating the EPC peak
            // budget, flush the current batch and reuse the same L1 table.
            flush();
        }

        // A new item is inserted only after growth has made the load safe.
        idx = hash(key) % l1_capacity_;
        for (uint64_t i = 0; i < l1_capacity_; ++i) {
            const uint64_t slot = (idx + i) % l1_capacity_;
            const SlotState existing_state = l1_state(slot);
            if (existing_state == SlotState::Empty) {
                assign_l1_slot(slot, entry, state);
                return;
            }
            if (l1_table_[static_cast<size_t>(slot)].key == key) {
                assign_l1_slot(slot, entry, state);
                return;
            }
        }
        printf("[FATAL] NoHashMap: L1 insertion failed after growth/flush\n");
        X_FAIL();
    }

    SlotState l1_find(uint64_t key, Entry& result) const {
        const uint64_t idx = hash(key) % l1_capacity_;
        for (uint64_t i = 0; i < l1_capacity_; ++i) {
            const uint64_t slot = (idx + i) % l1_capacity_;
            const SlotState state = l1_state(slot);
            if (state == SlotState::Empty) break;
            const Entry& existing = l1_table_[static_cast<size_t>(slot)];
            if (existing.key == key) {
                result = existing;
                return state;
            }
        }
        return SlotState::Empty;
    }

    // ========================================================================
    // Offline bulk-build helpers
    // ========================================================================

    void bulk_prepare_empty_l1(uint64_t target_capacity) {
        if (l1_count_ != 0 || target_capacity < l1_capacity_ ||
            target_capacity > l1_capacity_limit_) {
            printf("[FATAL] NoHashMap: invalid empty-L1 bulk resize\n");
            X_FAIL();
            return;
        }
        if (target_capacity == l1_capacity_) return;

        // Allocate the final table once.  This is deliberately separate from
        // rehash_l1: there are no old records to replay, and the online rehash
        // counters/path must remain untouched by an offline build.
        StdVec<Entry> new_table(target_capacity);
        StdVec<uint64_t> new_states(
            l1_state_word_count(target_capacity), uint64_t(0));
        l1_table_.swap(new_table);
        l1_states_.swap(new_states);
        l1_capacity_ = target_capacity;
        l1_peak_capacity_ = std::max(l1_peak_capacity_, target_capacity);
        buffer_real_count = l1_capacity_;
    }

    template <typename DatasetVec>
    void bulk_build_l1(DatasetVec& dataset) {
        const uint64_t input_count = dataset.size();
        uint64_t target_capacity = l1_capacity_;
        while (input_count > l1_load_limit(target_capacity)) {
            if (target_capacity >= l1_capacity_limit_) {
                printf("[FATAL] NoHashMap: bulk L1 target exceeds EPC limit\n");
                X_FAIL();
                return;
            }
            target_capacity = (target_capacity > l1_capacity_limit_ / 2)
                ? l1_capacity_limit_
                : target_capacity * 2;
        }
        bulk_prepare_empty_l1(target_capacity);

        typename DatasetVec::Reader reader(dataset.begin(), dataset.end());
        for (uint64_t i = 0; i < input_count; ++i) {
            const auto kv = reader.read();
            l1_insert(Entry{kv.key, kv.value}, SlotState::Live);
        }
        if (l2_data_ptr_ || l1_count_ > input_count) {
            printf("[FATAL] NoHashMap: offline L1 build entered online flush path\n");
            X_FAIL();
        }
    }

    void bulk_write_l2_bucket(uint64_t b,
                              const std::vector<BulkEntry>& entries,
                              uint64_t begin,
                              uint64_t end) {
        const uint64_t write_count = end - begin;
        if (write_count > items_per_bucket_) {
            printf("[FATAL] NoHashMap: bulk L2 bucket %llu overflow: "
                   "%llu items > %llu capacity\n",
                   (unsigned long long)b,
                   (unsigned long long)write_count,
                   (unsigned long long)items_per_bucket_);
            X_FAIL();
            return;
        }

        const uint64_t padded_write_count =
            ((write_count + kItemsPerPage - 1) / kItemsPerPage) *
            kItemsPerPage;
        if (padded_write_count > items_per_bucket_) {
            printf("[FATAL] NoHashMap: padded bulk L2 bucket %llu "
                   "overflow\n", (unsigned long long)b);
            X_FAIL();
            return;
        }

        const uint64_t l2_offset = b * items_per_bucket_;
        if (padded_write_count != 0) {
            uint32_t auth_version =
                bucket_auth_versions_[static_cast<size_t>(b)];
            if (auth_version == UINT32_MAX) {
                printf("[FATAL] NoHashMap: authentication version "
                       "overflow in bulk L2 bucket %llu\n",
                       (unsigned long long)b);
                X_FAIL();
                return;
            }
            ++auth_version;
            typename L2Vec::Writer writer(
                l2_data_ptr_->begin() + l2_offset,
                l2_data_ptr_->begin() + l2_offset + padded_write_count,
                auth_version);
            for (uint64_t i = begin; i < end; ++i) {
                writer.write(entries[static_cast<size_t>(i)].kv);
            }
            for (uint64_t i = write_count; i < padded_write_count; ++i) {
                writer.write(Entry{});
            }
            writer.flush();
            bucket_auth_versions_[static_cast<size_t>(b)] = auth_version;
        }

        bucket_sizes_[static_cast<size_t>(b)] = write_count;
        for (uint64_t p = 0; p < pages_per_bucket_; ++p) {
            const uint64_t item_idx = p * kItemsPerPage;
            const uint64_t page_idx = b * pages_per_bucket_ + p;
            page_index_[static_cast<size_t>(page_idx)] =
                (item_idx < write_count)
                    ? entries[static_cast<size_t>(begin + item_idx)].kv.key
                    : UINT64_MAX;
        }
        bf_clear(b);
        for (uint64_t i = begin; i < end; ++i) {
            bf_add(b, entries[static_cast<size_t>(i)].kv.key);
        }
    }

    template <typename DatasetVec>
    void bulk_build_l2(DatasetVec& dataset) {
        const uint64_t input_count = dataset.size();
        ensure_l2();

        // One counting pass lets each later pass scatter directly into
        // contiguous per-bucket slices; only each bucket, not the full input,
        // needs sorting.  Bucket ranges are chosen to keep transient records
        // inside a conservative fraction of the available EPC.
        flush_counts_.assign(num_buckets_, 0);
        {
            typename DatasetVec::Reader reader(dataset.begin(), dataset.end());
            for (uint64_t i = 0; i < input_count; ++i) {
                const auto kv = reader.read();
                ++flush_counts_[hash(kv.key) % num_buckets_];
            }
        }

        uint64_t l1_budget_bytes = 0;
        if (!checked_mul_u64(l1_slot_budget_, kL1BudgetBytesPerSlot,
                             l1_budget_bytes)) {
            printf("[FATAL] NoHashMap: bulk scratch budget overflow\n");
            X_FAIL();
            return;
        }
        const uint64_t scratch_bytes =
            std::min(EnclaveSize / 4, l1_budget_bytes / 2);
        const uint64_t max_chunk_entries =
            std::max<uint64_t>(1, scratch_bytes / sizeof(BulkEntry));

        for (uint64_t cb = 0; cb < num_buckets_;) {
            uint64_t cb_end = cb;
            uint64_t chunk_count = 0;
            while (cb_end < num_buckets_) {
                const uint64_t bucket_count = flush_counts_[cb_end];
                if (cb_end > cb &&
                    (chunk_count >= max_chunk_entries ||
                     bucket_count > max_chunk_entries - chunk_count)) {
                    break;
                }
                chunk_count += bucket_count;
                ++cb_end;
                if (chunk_count >= max_chunk_entries) break;
            }
            if (cb_end == cb) {
                // A single unusually skewed bucket must remain intact so its
                // records can be sorted and de-duplicated together.
                chunk_count = flush_counts_[cb];
                ++cb_end;
            }
            if (chunk_count == 0) {
                cb = cb_end;
                continue;
            }

            const uint64_t bucket_count = cb_end - cb;
            std::vector<uint64_t> offsets(
                static_cast<size_t>(bucket_count + 1), 0);
            std::vector<uint64_t> fills(static_cast<size_t>(bucket_count), 0);
            for (uint64_t local = 0; local < bucket_count; ++local) {
                offsets[static_cast<size_t>(local + 1)] =
                    offsets[static_cast<size_t>(local)] +
                    flush_counts_[cb + local];
            }

            std::vector<BulkEntry> chunk(static_cast<size_t>(chunk_count));
            {
                typename DatasetVec::Reader reader(dataset.begin(), dataset.end());
                for (uint64_t sequence = 0; sequence < input_count; ++sequence) {
                    const auto kv = reader.read();
                    const uint64_t b = hash(kv.key) % num_buckets_;
                    if (b < cb || b >= cb_end) continue;
                    const uint64_t local = b - cb;
                    const uint64_t pos =
                        offsets[static_cast<size_t>(local)] +
                        fills[static_cast<size_t>(local)]++;
                    if (pos >= chunk_count) {
                        printf("[FATAL] NoHashMap: bulk scatter overflow\n");
                        X_FAIL();
                        return;
                    }
                    chunk[static_cast<size_t>(pos)] =
                        BulkEntry{Entry{kv.key, kv.value}, sequence};
                }
            }

            for (uint64_t local = 0; local < bucket_count; ++local) {
                if (fills[static_cast<size_t>(local)] !=
                    flush_counts_[cb + local]) {
                    printf("[FATAL] NoHashMap: bulk scatter count mismatch\n");
                    X_FAIL();
                    return;
                }
                const uint64_t begin = offsets[static_cast<size_t>(local)];
                const uint64_t end = offsets[static_cast<size_t>(local + 1)];
                if (begin == end) continue;

                std::sort(chunk.begin() + begin, chunk.begin() + end,
                          [](const BulkEntry& a, const BulkEntry& b) {
                              if (a.kv.key != b.kv.key)
                                  return a.kv.key < b.kv.key;
                              return a.sequence < b.sequence;
                          });

                // Compact equal keys in place, retaining the last input
                // occurrence.  The unused tail stays within this bucket's
                // original slice and cannot disturb a following bucket.
                uint64_t unique_end = begin;
                for (uint64_t read = begin; read < end;) {
                    uint64_t next = read + 1;
                    while (next < end &&
                           chunk[static_cast<size_t>(next)].kv.key ==
                               chunk[static_cast<size_t>(read)].kv.key) {
                        ++next;
                    }
                    chunk[static_cast<size_t>(unique_end++)] =
                        chunk[static_cast<size_t>(next - 1)];
                    read = next;
                }
                bulk_write_l2_bucket(cb + local, chunk, begin, unique_end);
            }
            cb = cb_end;
        }

        // Counting storage is only bulk scratch.  Do not leave it charged to
        // the persistent map or alter the online flush buffer lifecycle.
        std::vector<uint64_t>().swap(flush_counts_);
    }

    // ========================================================================
    // Batched flush: L1 → L2
    // ========================================================================

    void flush() {
        const uint64_t nb = num_buckets_;
        const uint64_t n = l1_count_;
        if (n == 0) return;
        ++l1_flush_count_;
        ensure_l2();

        // Pass 1: count items per bucket
        flush_counts_.assign(nb, 0);
        for (uint64_t i = 0; i < l1_capacity_; ++i) {
            if (l1_state(i) == SlotState::Empty) continue;
            const Entry& entry = l1_table_[static_cast<size_t>(i)];
            ++flush_counts_[hash(entry.key) % nb];
        }

        // Chunked flush: process K buckets at a time to limit EPC peak
        // Each chunk: scatter items for K buckets → merge with L2 → write back
        const uint64_t chunk_size = std::max(uint64_t(1), nb / 8);  // ~8 chunks

        for (uint64_t cb = 0; cb < nb; cb += chunk_size) {
            const uint64_t cb_end = std::min(cb + chunk_size, nb);

            // Compute total items in this chunk
            uint64_t chunk_items = 0;
            for (uint64_t b = cb; b < cb_end; ++b) {
                chunk_items += flush_counts_[b];
            }
            if (chunk_items == 0) continue;

            // Scatter items for this chunk's buckets into a small sorted buffer
            flush_sorted_.resize(chunk_items);
            flush_fill_.assign(nb, 0);
            // Chunk-local offsets (rebased to 0)
            std::vector<uint64_t> chunk_offsets(nb, 0);
            {
                uint64_t acc = 0;
                for (uint64_t b = cb; b < cb_end; ++b) {
                    chunk_offsets[b] = acc;
                    acc += flush_counts_[b];
                }
            }
            for (uint64_t i = 0; i < l1_capacity_; ++i) {
                const SlotState state = l1_state(i);
                if (state == SlotState::Empty) continue;
                const Entry& entry = l1_table_[static_cast<size_t>(i)];
                uint64_t b = hash(entry.key) % nb;
                if (b >= cb && b < cb_end) {
                    flush_sorted_[chunk_offsets[b] + flush_fill_[b]] =
                        PendingEntry{entry, state};
                    ++flush_fill_[b];
                }
            }

            // Process each bucket in this chunk
            for (uint64_t b = cb; b < cb_end; ++b) {
                if (flush_counts_[b] == 0) continue;

                uint64_t bucket_start = chunk_offsets[b];
                uint64_t bucket_end = bucket_start + flush_counts_[b];
                uint64_t l2_offset = b * items_per_bucket_;
                uint64_t sz = bucket_sizes_[static_cast<size_t>(b)];

                // Sort L1 items for this bucket by key (for two-pointer merge)
                std::sort(flush_sorted_.begin() + bucket_start,
                          flush_sorted_.begin() + bucket_end,
                          [](const PendingEntry& a, const PendingEntry& b) {
                              return a.kv.key < b.kv.key;
                          });

                // Read existing L2 bucket (already sorted by key)
                StdVec<Entry> l2_existing;
                if (sz > 0) {
                    l2_existing.reserve(sz);
                    typename L2Vec::Reader r(
                        l2_data_ptr_->begin() + l2_offset,
                        l2_data_ptr_->begin() + l2_offset + sz,
                        bucket_auth_versions_[static_cast<size_t>(b)]);
                    for (uint64_t j = 0; j < sz; ++j) {
                        l2_existing.push_back(r.read());
                    }
                }

                // Two-pointer merge: O(M + K) instead of O(M * K)
                StdVec<Entry> merged;
                merged.reserve(sz + flush_counts_[b]);

                uint64_t p1 = bucket_start;
                uint64_t p2 = 0;

                while (p1 < bucket_end && p2 < l2_existing.size()) {
                    const PendingEntry& l1_item = flush_sorted_[p1];
                    const Entry& l2_item = l2_existing[p2];

                    if (l1_item.kv.key < l2_item.key) {
                        if (l1_item.state == SlotState::Live) {
                            merged.push_back(l1_item.kv);
                        }
                        ++p1;
                    } else if (l1_item.kv.key > l2_item.key) {
                        merged.push_back(l2_item);
                        ++p2;
                    } else {
                        // L1 is newer.  A delete removes the old L2 record.
                        if (l1_item.state == SlotState::Live) {
                            merged.push_back(l1_item.kv);
                        }
                        ++p1;
                        ++p2;
                    }
                }
                while (p1 < bucket_end) {
                    const PendingEntry& item = flush_sorted_[p1++];
                    if (item.state == SlotState::Live) {
                        merged.push_back(item.kv);
                    }
                }
                while (p2 < l2_existing.size()) {
                    merged.push_back(l2_existing[p2++]);
                }

                // Write back to L2
                if (merged.size() > items_per_bucket_) {
                    printf("[FATAL] NoHashMap: L2 bucket %llu overflow: "
                           "%llu merged items > %llu capacity\n",
                           (unsigned long long)b,
                           (unsigned long long)merged.size(),
                           (unsigned long long)items_per_bucket_);
                    X_FAIL();
                    __builtin_unreachable();
                }
                const uint64_t write_count =
                    static_cast<uint64_t>(merged.size());
                const uint64_t padded_write_count =
                    ((write_count + kItemsPerPage - 1) / kItemsPerPage) *
                    kItemsPerPage;
                if (padded_write_count > items_per_bucket_) {
                    printf("[FATAL] NoHashMap: padded L2 bucket %llu "
                           "overflow: %llu items > %llu capacity\n",
                           (unsigned long long)b,
                           (unsigned long long)padded_write_count,
                           (unsigned long long)items_per_bucket_);
                    X_FAIL();
                    __builtin_unreachable();
                }
                if (padded_write_count != 0) {
                    uint32_t auth_version =
                        bucket_auth_versions_[static_cast<size_t>(b)];
                    if (auth_version == UINT32_MAX) {
                        printf("[FATAL] NoHashMap: authentication version "
                               "overflow in L2 bucket %llu\n",
                               (unsigned long long)b);
                        X_FAIL();
                        __builtin_unreachable();
                    }
                    ++auth_version;
                    {
                        typename L2Vec::Writer w(
                            l2_data_ptr_->begin() + l2_offset,
                            l2_data_ptr_->begin() + l2_offset +
                                padded_write_count,
                            auth_version);
                        for (uint64_t j = 0; j < write_count; ++j) {
                            w.write(merged[static_cast<size_t>(j)]);
                        }
                        for (uint64_t j = write_count;
                             j < padded_write_count; ++j) {
                            w.write(Entry{});
                        }
                        w.flush();
                    }
                    bucket_auth_versions_[static_cast<size_t>(b)] =
                        auth_version;
                }

                // Update metadata
                bucket_sizes_[static_cast<size_t>(b)] = write_count;

                // Build page-level index for this bucket (first key per page)
                for (uint64_t p = 0; p < pages_per_bucket_; ++p) {
                    uint64_t item_idx = p * kItemsPerPage;
                    uint64_t idx = b * pages_per_bucket_ + p;
                    if (item_idx < write_count) {
                        page_index_[static_cast<size_t>(idx)] =
                            merged[static_cast<size_t>(item_idx)].key;
                    } else {
                        page_index_[static_cast<size_t>(idx)] = UINT64_MAX;
                    }
                }
                bf_clear(b);
                for (uint64_t j = 0; j < write_count; ++j) {
                    bf_add(b, merged[static_cast<size_t>(j)].key);
                }
            }
        }

        // Clear L1
        std::fill(l1_states_.begin(), l1_states_.end(), uint64_t(0));
        l1_count_ = 0;
        l1_deleted_count_ = 0;

        // flush_sorted_ can be as large as the pending L1 batch.  Release its
        // capacity after each flush so a previous large batch cannot reduce
        // the EPC available to subsequent L1 growth.
        std::vector<PendingEntry>().swap(flush_sorted_);
    }

    // ========================================================================
    // Public API
    // ========================================================================

    void flush_if_l1_limit_reached() {
        if (l1_capacity_ >= l1_capacity_limit_ &&
            l1_count_ >= l1_load_limit(l1_capacity_)) {
            flush();
        }
    }

    void Insert(uint64_t key, uint64_t value) {
        l1_insert(Entry{key, value}, SlotState::Live);
        flush_if_l1_limit_reached();
    }

    void Delete(uint64_t key) {
        l1_insert(Entry{key, 0}, SlotState::Deleted);
        flush_if_l1_limit_reached();
    }

    Item PointQuery(uint64_t key) {
        // Step 1: Probe L1 (enclave, 0 external I/O)
        Entry l1_result{};
        const SlotState l1_result_state = l1_find(key, l1_result);
        if (l1_result_state == SlotState::Deleted) return Item();
        if (l1_result_state == SlotState::Live)
            return Item(OpType::INS, l1_result.key, l1_result.value, 0);

        // Step 2: Bloom filter check (enclave, 0 external I/O)
        uint64_t b = hash(key) % num_buckets_;
        if (!bf_maybe_contains(b, key)) {
            return Item();
        }

        // Step 3: Page-level binary search + single page read
        uint64_t offset = b * items_per_bucket_;
        uint64_t sz = bucket_sizes_[static_cast<size_t>(b)];
        if (sz == 0) return Item();

        ensure_l2();

        // Binary search in page index to find target page
        uint64_t page_base = b * pages_per_bucket_;
        uint64_t target_page = 0;
        {
            const uint64_t valid_pages =
                (sz + kItemsPerPage - 1) / kItemsPerPage;
            uint64_t lo = 0, hi = valid_pages;
            while (lo < hi) {
                uint64_t mid = lo + (hi - lo) / 2;
                uint64_t first_key = page_index_[static_cast<size_t>(page_base + mid)];
                if (first_key > key) {
                    hi = mid;
                } else {
                    lo = mid + 1;
                }
            }
            target_page = (lo > 0) ? lo - 1 : 0;
        }

        // Read only the target page (1 page I/O)
        uint64_t page_start = target_page * kItemsPerPage;
        uint64_t page_end = std::min(page_start + kItemsPerPage, sz);
        if (page_start >= sz) return Item();

        typename L2Vec::Reader r(
            l2_data_ptr_->begin() + offset + page_start,
            l2_data_ptr_->begin() + offset + page_end,
            bucket_auth_versions_[static_cast<size_t>(b)]);
        for (uint64_t j = page_start; j < page_end; ++j) {
            const Entry entry = r.read();
            if (entry.key == key) {
                return Item(OpType::INS, entry.key, entry.value, 0);
            }
            if (entry.key > key) break;
        }

        return Item();
    }

    // ========================================================================
    // Diagnostics for benchmark reports
    // ========================================================================

    uint64_t l1_live_count() const { return l1_count_ - l1_deleted_count_; }
    uint64_t l1_occupied_count() const { return l1_count_; }
    uint64_t l1_deleted_count() const { return l1_deleted_count_; }
    uint64_t l1_capacity() const { return l1_capacity_; }
    uint64_t l1_capacity_limit() const { return l1_capacity_limit_; }
    uint64_t l1_slot_budget() const { return l1_slot_budget_; }
    uint64_t l1_peak_capacity() const { return l1_peak_capacity_; }
    uint64_t l1_rehash_count() const { return l1_rehash_count_; }
    uint64_t l1_flush_count() const { return l1_flush_count_; }
    bool l2_allocated() const { return l2_data_ptr_ != nullptr; }

    RangeQueryResult RangeQuery(uint64_t lo, uint64_t hi) {
        StdVec<Item> result;
        if (lo > hi) {
            return RangeQueryResult(std::move(result));
        }

        // A hash table has no ordered traversal.  The benchmark's inclusive
        // numeric interval is therefore emulated by independent point lookups,
        // preserving exactly the same update/delete semantics as PointQuery.
        for (uint64_t key = lo;; ++key) {
            Item item = PointQuery(key);
            if (!item.is_dummy()) {
                result.push_back(item);
            }
            if (key == hi) break;  // also prevents UINT64_MAX wraparound
        }

        return RangeQueryResult(std::move(result));
    }

    template <typename DatasetVec>
    void Ingest(DatasetVec& dataset) {
        if (dataset.size() == 0) return;

        // Ingest is an offline, one-shot constructor.  Rejecting a non-empty
        // target prevents it from silently becoming a second update path and
        // keeps all incremental semantics exclusively in Insert/Delete/flush.
        bool has_l2_records = false;
        for (uint64_t b = 0; b < num_buckets_; ++b) {
            if (bucket_sizes_[static_cast<size_t>(b)] != 0) {
                has_l2_records = true;
                break;
            }
        }
        if (l1_count_ != 0 || has_l2_records) {
            printf("[FATAL] NoHashMap: Ingest requires an empty map\n");
            X_FAIL();
            return;
        }

        if (dataset.size() <= l1_load_limit(l1_capacity_limit_)) {
            bulk_build_l1(dataset);
        } else {
            bulk_build_l2(dataset);
        }
    }

    // ========================================================================
    // Memory accounting
    // ========================================================================

    uint64_t getByteSize() const {
        uint64_t bytes = sizeof(*this);
        auto add_allocation = [&](uint64_t count, uint64_t width) {
            uint64_t allocation = 0;
            return checked_mul_u64(count, width, allocation) &&
                   checked_add_u64(bytes, allocation, bytes);
        };
        if (!add_allocation(l1_table_.capacity(), sizeof(Entry)) ||
            !add_allocation(l1_states_.capacity(), sizeof(uint64_t)) ||
            !add_allocation(bucket_sizes_.capacity(), sizeof(uint64_t)) ||
            !add_allocation(bucket_auth_versions_.capacity(), sizeof(uint32_t)) ||
            !add_allocation(bf_data_.capacity(), sizeof(uint64_t)) ||
            !add_allocation(page_index_.capacity(), sizeof(uint64_t)) ||
            !add_allocation(flush_counts_.capacity(), sizeof(uint64_t)) ||
            !add_allocation(flush_fill_.capacity(), sizeof(uint64_t)) ||
            !add_allocation(flush_sorted_.capacity(), sizeof(PendingEntry))) {
            return UINT64_MAX;
        }

        if (l2_data_ptr_) {
            using Page = typename L2Vec::Page;
            using EncPage = typename Page::Encrypted_t;
            uint64_t total_l2 = 0;
            if (!checked_mul_u64(num_buckets_, items_per_bucket_, total_l2)) {
                return UINT64_MAX;
            }
            const uint64_t items_per_page = L2Vec::item_per_page;
            const uint64_t num_pages = total_l2 / items_per_page +
                ((total_l2 % items_per_page) != 0 ? 1ULL : 0ULL);
            if (!add_allocation(num_pages, sizeof(EncPage))) {
                return UINT64_MAX;
            }
            const uint64_t modified_bits =
                static_cast<uint64_t>(l2_data_ptr_->server.modified.capacity());
            const uint64_t modified_bytes =
                modified_bits / 8ULL +
                ((modified_bits % 8ULL) != 0 ? 1ULL : 0ULL);
            if (!checked_add_u64(bytes, modified_bytes, bytes)) {
                return UINT64_MAX;
            }
        }

        return bytes;
    }

    // ========================================================================
    // Level interface (required by benchmark driver)
    // ========================================================================

    uint64_t level_count() const { return 2; }

    bool level_occupied(uint64_t l) const {
        return l == 1 || (l == 2 && l2_data_ptr_ != nullptr);
    }
    bool level_in_enclave(uint64_t l) const { return l == 1; }
};

}  // namespace EM::Sapphmap
