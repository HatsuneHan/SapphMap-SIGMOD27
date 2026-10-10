#pragma once

/**
 * @file enigmap_index_adapter.hpp
 * @brief Adapter that exposes EnigMap with the SapphMap benchmark interface.
 *
 * The adapter normalizes EnigMap operations (insert/query/range/delete,
 * capacity accounting, and size reporting) so benchmark code can switch between
 * SapphMap, KVLSMTree, and EnigMap via a common template contract.
 */

#include <cstdint>

#include "external_memory/noncachedvector.hpp"
#include "external_memory/sapphmap/pds_def.hpp"
#include "external_memory/tools/enigmap/enigmap_obst.hpp"

template <uint64_t CPS, uint64_t ES>
/**
 * @brief Compatibility wrapper for EnigMap in benchmark templates.
 */
struct EnigMapIndexAdapter {
    static constexpr bool kSupportsExp1BulkBuild = false;

    using KvPair = EM::Sapphmap::KvPair;
    template <typename T>
    using NCVec = EM::NonCachedVector::Vector<T>;

    EnigMapCompat::OBST_t obst;
    uint64_t enclave_levels = 0;
    uint64_t buffer_real_count = 0;

    explicit EnigMapIndexAdapter(uint64_t max_writes)
        : obst((max_writes == 0) ? 1ULL : max_writes, /*noInit=*/true) {}

    /// Bulk ingest through repeated EnigMap inserts.
    void Ingest(NCVec<KvPair>& dataset) {
        typename NCVec<KvPair>::Reader reader(dataset.begin(), dataset.end());
        for (uint64_t i = 0; i < dataset.size(); ++i) {
            const KvPair kv = reader.read();
            obst.Insert(kv.key, kv.value);
        }
    }

    /// Point insert forwarded to EnigMap.
    void Insert(uint64_t k, uint64_t v) {
        obst.Insert(k, v);
    }

    /// Point query forwarded to EnigMap; return value is intentionally ignored by benchmark driver.
    void PointQuery(uint64_t k) {
        EnigMapCompat::V out = 0;
        obst.Get(k, out);
        (void)out;
    }

    /// Range query emulation via repeated point queries.
    void RangeQuery(uint64_t left, uint64_t right) {
        if (left > right) return;
        for (uint64_t k = left;; ++k) {
            PointQuery(k);
            if (k == right) break;
        }
    }

    void Delete(uint64_t k) {
        // EnigMap OBST has no native delete; approximate benchmark behavior by
        // writing a deterministic tombstone-like value.
        obst.Insert(k, 100);
    }

    uint64_t level_count() const {
        return obst.oram.L_ + 1;
    }

    uint64_t getByteSize() const {
        using Block_t = typename EnigMapCompat::ORAMClient_t::Block_t;
        return 2ULL * obst.oram.N_ * ORAM__Z * sizeof(Block_t);
    }

    // Exp1 reporting compatibility: report logical tree size for N writes even
    // when internal capacity is provisioned larger for replay scenarios.
    static uint64_t ComputeReportedTreeBytesByWrites(uint64_t writes) {
        using Block_t = typename EnigMapCompat::ORAMClient_t::Block_t;
        const uint64_t safe_writes = (writes == 0) ? 1ULL : writes;
        const uint64_t logical_oram_n = safe_writes + 2ULL;
        return 2ULL * logical_oram_n * ORAM__Z * sizeof(Block_t);
    }

    /// Compute external backend bytes for an EnigMap ORAM tree sized by @p n.
    static uint64_t ComputeBackendBytes(uint64_t n) {
        using LB_t = typename EnigMapCompat::ORAMClient_t::ORAMClientInterface_t::LargeBucket_t;
        constexpr uint64_t LPP = ORAM_SERVER__LEVELS_PER_PACK;
        constexpr uint64_t BPP = LB_t::BUCKETS_PER_PACK;
        const uint64_t oram_n = n + 2;
        const uint64_t oram_l = CeilLog2(oram_n);
        const uint64_t oram_v = 1ULL << ((oram_l + 1 + LPP - 1) / LPP * LPP);
        const uint64_t num_lb = oram_v / BPP + 2;
        return static_cast<uint64_t>(num_lb * sizeof(LB_t) * 1.2) + (1ULL << 28);
    }

    /// Recommend backend bytes for benchmark mode-specific provisioning.
    static uint64_t RecommendBackendSize(uint64_t init_size,
                                         uint64_t max_writes,
                                         bool is_exp1_split) {
        const uint64_t base_n = ((init_size > max_writes) ? init_size : max_writes) + 1;
        if (is_exp1_split) {
            const uint64_t seq_n = init_size * 2 + 1024;
            const uint64_t seq_backend = ComputeBackendBytes(seq_n);
            return seq_backend + seq_backend / 2;
        }
        return ComputeBackendBytes(base_n);
    }
};
