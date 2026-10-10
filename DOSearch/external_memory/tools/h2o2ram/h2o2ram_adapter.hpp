#pragma once

/**
 * @file h2o2ram_adapter.hpp
 * @brief H2O2RAM adapter for SapphMap benchmark interface.
 *
 * Uses H2O2RAM's ObliviousMap for storage (the real ORAM implementation).
 * Follows the EnigMap adapter pattern: NCVec = NonCachedVector for Ingest,
 * ObliviousMap for Insert/PointQuery/RangeQuery/Delete.
 */

#include <cstdint>
#include <cstdlib>

#include "external_memory/noncachedvector.hpp"
#include "external_memory/sapphmap/pds_def.hpp"
#include "external_memory/tools/h2o2ram/omap.hpp"

template <uint64_t CachePageSize, uint64_t EnclaveSize>
struct H2O2RAMAdapter {
    static constexpr bool kSupportsExp1BulkBuild = false;
    static constexpr uint64_t kBlockBytes = sizeof(uint64_t) + sizeof(uint64_t);
    static constexpr uint64_t kLinearScanBase = LINEAR_SCAN_THRESHOLD;
    static constexpr uint64_t kMinCapacity = MIN_CAPACITY;
    static constexpr uint64_t kMaxSupportedWrites = 1ULL << 30;
    static constexpr uint64_t kLinearScanBufferItems = 2ULL * kLinearScanBase;
    static constexpr uint64_t kExtVectorPageBytes = std::max((1UL << 12) - 32, kBlockBytes);
    static constexpr uint64_t kBlockItemsPerPage = kExtVectorPageBytes / kBlockBytes;
    static constexpr uint64_t kPairBytes = sizeof(uint64_t) + kBlockBytes;
    static constexpr uint64_t kPairItemsPerPage = kExtVectorPageBytes / kPairBytes;
    // FreshEncrypted adds a 32-byte tag, making the usual 4064-byte payload
    // page exactly one 4096-byte backend allocation.
    static constexpr uint64_t kEncryptedPageBytes = 1ULL << 12;

    using KvPair = EM::Sapphmap::KvPair;
    template <typename T>
    using NCVec = EM::NonCachedVector::Vector<T>;

    h2o2ram_ns::ObliviousMap<uint64_t, uint64_t> omap;
    uint64_t max_writes_;
    uint64_t write_calls_ = 0;
    uint64_t enclave_levels = 0;
    uint64_t buffer_real_count = 0;

    explicit H2O2RAMAdapter(uint64_t max_writes)
        : max_writes_((max_writes == 0) ? 1ULL : max_writes) {
        if (max_writes_ > kMaxSupportedWrites) {
            printf("[H2O2RAM ERROR] requested writes=%llu exceed supported maximum=%llu\n",
                   static_cast<unsigned long long>(max_writes_),
                   static_cast<unsigned long long>(kMaxSupportedWrites));
            std::abort();
        }
    }

    void ensure_write_capacity(uint64_t additional) const {
        if (write_calls_ > max_writes_ || additional > max_writes_ - write_calls_) {
            printf("[H2O2RAM ERROR] write capacity exceeded: completed=%llu additional=%llu limit=%llu\n",
                   static_cast<unsigned long long>(write_calls_),
                   static_cast<unsigned long long>(additional),
                   static_cast<unsigned long long>(max_writes_));
            std::abort();
        }
    }

    static uint64_t round_up_pow2(uint64_t value) {
        if (value <= 1ULL) {
            return 1ULL;
        }
        if (value > (1ULL << 63)) {
            return 0;
        }
        --value;
        value |= value >> 1;
        value |= value >> 2;
        value |= value >> 4;
        value |= value >> 8;
        value |= value >> 16;
        value |= value >> 32;
        return value + 1ULL;
    }

    static uint64_t extvector_bytes(uint64_t items, uint64_t items_per_page) {
        if (items_per_page == 0) {
            return UINT64_MAX;
        }
        const uint64_t whole_pages = items / items_per_page;
        if (whole_pages == UINT64_MAX) {
            return UINT64_MAX;
        }
        const uint64_t pages = whole_pages + 1ULL;
        if (pages > (UINT64_MAX / kEncryptedPageBytes)) {
            return UINT64_MAX;
        }
        return pages * kEncryptedPageBytes;
    }

    static uint64_t entries_bytes_for_level(uint64_t level_capacity) {
        if (level_capacity > UINT64_MAX / 2ULL) {
            return UINT64_MAX;
        }
        const uint64_t items = 2ULL * level_capacity;
        return extvector_bytes(items, kBlockItemsPerPage);
    }

    static uint64_t build_bytes_for_level(uint64_t level_capacity) {
        if (level_capacity > UINT64_MAX / 3ULL) {
            return UINT64_MAX;
        }
        const uint64_t tmp_bytes = extvector_bytes(3ULL * level_capacity, kPairItemsPerPage);
        const uint64_t entries_bytes = entries_bytes_for_level(level_capacity);
        if (tmp_bytes == UINT64_MAX || entries_bytes == UINT64_MAX) {
            return UINT64_MAX;
        }
        if (tmp_bytes > UINT64_MAX - entries_bytes) {
            return UINT64_MAX;
        }
        return tmp_bytes + entries_bytes;
    }

    static uint64_t ComputeBackendBytes(uint64_t writes) {
        const uint64_t safe_writes = (writes == 0) ? 1ULL : writes;
        if (safe_writes > kMaxSupportedWrites) return UINT64_MAX;
        const uint64_t logical_writes = (safe_writes < kMinCapacity) ? kMinCapacity : safe_writes;
        const uint64_t rounded_capacity = round_up_pow2(logical_writes);
        if (rounded_capacity == 0) {
            return UINT64_MAX;
        }
        const uint64_t top_capacity = std::max(rounded_capacity, kMinCapacity);

        // Capacity-planning upper bound.  The linear scan buffer is enclave
        // resident; external levels begin above SMALL_HASH_TABLE_THRESHOLD.
        uint64_t steady_bytes = 0;

        for (uint64_t level_capacity = kLinearScanBase; level_capacity <= top_capacity; level_capacity <<= 1) {
            const uint64_t level_bytes = entries_bytes_for_level(level_capacity);
            if (level_bytes == UINT64_MAX) {
                return UINT64_MAX;
            }
            if (steady_bytes > UINT64_MAX - level_bytes) {
                return UINT64_MAX;
            }
            steady_bytes += level_bytes;
            if (level_capacity == top_capacity) {
                break;
            }
            if (level_capacity > (UINT64_MAX >> 1)) {
                return UINT64_MAX;
            }
        }

        const uint64_t peak_build_bytes = build_bytes_for_level(top_capacity);
        if (peak_build_bytes == UINT64_MAX || steady_bytes > UINT64_MAX - peak_build_bytes) {
            return UINT64_MAX;
        }
        return steady_bytes + peak_build_bytes;
    }

    static uint64_t RecommendBackendSize(uint64_t init_size,
                                         uint64_t max_writes,
                                         bool is_exp1_split) {
        const uint64_t base_writes = (init_size > max_writes) ? init_size : max_writes;
        const uint64_t backend = ComputeBackendBytes(base_writes);
        if (backend == UINT64_MAX || backend > UINT64_MAX - backend / 2) {
            return UINT64_MAX;
        }
        // Both split and replay modes need construction headroom.  The common
        // replay path adds this 50% itself; split mode calls us directly.
        return is_exp1_split ? (backend + backend / 2) : backend;
    }

    void Ingest(NCVec<KvPair>& dataset) {
        ensure_write_capacity(dataset.size());
        typename NCVec<KvPair>::Reader reader(dataset.begin(), dataset.end());
        for (uint64_t i = 0; i < dataset.size(); ++i) {
            const KvPair kv = reader.read();
            omap.insert(kv.key, kv.value);
        }
        write_calls_ += dataset.size();
    }

    // Pure insertion for initialization traces, whose keys are generated
    // unique.  Avoiding an unnecessary lookup preserves upstream build cost.
    void InsertNew(uint64_t k, uint64_t v) {
        ensure_write_capacity(1);
        omap.insert(k, v);
        ++write_calls_;
    }

    // Benchmark PUT is an upsert.  Upstream insert() assumes a fresh key, so
    // remove any prior version obliviously before appending the replacement.
    void Insert(uint64_t k, uint64_t v) {
        ensure_write_capacity(1);
        omap.erase(k);
        omap.insert(k, v);
        ++write_calls_;
    }

    uint64_t PointQuery(uint64_t k) { return omap[k]; }

    void RangeQuery(uint64_t left, uint64_t right) {
        if (left > right) return;
        for (uint64_t k = left;; ++k) {
            PointQuery(k);
            if (k == right) break;
        }
    }

    void Delete(uint64_t k) { omap.erase(k); }

    void Flush() { omap.flush(); }

    using Stats = typename h2o2ram_ns::ObliviousMap<uint64_t, uint64_t>::Stats;
    using Diagnostics = typename h2o2ram_ns::ObliviousMap<uint64_t, uint64_t>::Diagnostics;

    uint64_t level_count() const { return omap.level_count(); }
    uint64_t write_calls() const { return write_calls_; }
    Stats stats() const { return omap.stats(); }
    void reset_stats() const { omap.reset_stats(); }
    Diagnostics diagnostics() const { return omap.diagnostics(); }
    uint64_t level_capacity(uint64_t level) const {
        return omap.level_capacity(static_cast<size_t>(level));
    }
    bool level_occupied(uint64_t level) const {
        return omap.level_occupied(static_cast<size_t>(level));
    }

    uint64_t getByteSize() const {
        // Report steady physical external allocation, not the construction
        // peak used to provision the server.  Benchmark trace vectors have
        // left scope at every call site, so the remaining allocation belongs
        // to the index.
        if (EM::Backend::g_DefaultBackend != nullptr) {
            uint64_t free_bytes = 0;
            for (const auto &slot : EM::Backend::g_DefaultBackend->freeList) {
                if (slot.size > UINT64_MAX - free_bytes) return UINT64_MAX;
                free_bytes += slot.size;
            }
            if (free_bytes <= EM::Backend::g_DefaultBackend->size) {
                return EM::Backend::g_DefaultBackend->size - free_bytes;
            }
        }
        return ComputeBackendBytes(max_writes_);
    }
};
