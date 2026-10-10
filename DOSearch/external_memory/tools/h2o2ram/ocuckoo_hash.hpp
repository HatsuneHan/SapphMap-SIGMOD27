#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <memory>
#include "ohash_base.hpp"
#include "obipartite_matching.hpp"
#include "ocompact.hpp"
#include "oshuffle.hpp"
#include "prf.hpp"

#include "timer.hpp"

namespace h2o2ram_ns {

    template <typename KeyType,
              std::size_t BlockSize = sizeof(KeyType)>
    class OCuckooHash : public OHashBase<KeyType, BlockSize>
    {
    private:
        using Block_t = Block<KeyType, BlockSize>;
        using ExtVec_t = typename OHashBase<KeyType, BlockSize>::ExtVec_t;
        using PairBlock_t = PairBlock<KeyType, BlockSize>;
        using PairVec_t = std::vector<PairBlock_t>;

        KeyType n;
        KeyType delta_inv_log2;
        uint32_t prf_cnt;
        KeyType bucket_size = 0;
        // Hash the complete logical key.  Using a 32-bit PRF here aliases
        // distinct 64-bit YCSB keys before cuckoo placement and can make a
        // perfect matching impossible, regardless of the random PRF keys.
        std::vector<AESPRF<KeyType>> prfs;
        std::unique_ptr<ExtVec_t> entries;
        KeyType entries_capacity = 0;

        void compute_prf_cnt(const KeyType delta_inv_log2)
        {
            if (delta_inv_log2 <= 64)
            {
                if (n < 8)
                    prf_cnt = 3;
                if (n < 32)
                    prf_cnt = 5;
                else if (n < 1024)
                    prf_cnt = 4;
                else
                    prf_cnt = 3;
            }
            else if (delta_inv_log2 <= 96)
            {
                if (n < 8)
                    prf_cnt = 3;
                if (n < 32)
                    prf_cnt = 6;
                else if (n < 256)
                    prf_cnt = 5;
                else if (n < 65536)
                    prf_cnt = 4;
                else
                    prf_cnt = 3;
            }
            else
            {
                if (n < 8)
                    prf_cnt = 3;
                if (n < 16)
                    prf_cnt = 7;
                else if (n < 64)
                    prf_cnt = 6;
                else if (n < 2048)
                    prf_cnt = 5;
                else if (n < 2097152)
                    prf_cnt = 4;
                else
                    prf_cnt = 3;
            }
        }

    public:
        OCuckooHash(KeyType n,
                    KeyType delta_inv_log2 = DELTA_INV_LOG2)
            : n(n), delta_inv_log2(delta_inv_log2)
        {
            compute_prf_cnt(delta_inv_log2);
        }

        virtual void build(Block_t *data)
        {
            prfs.clear();
            bucket_size = 2 * n / prf_cnt;
            for (uint32_t i = 0; i < prf_cnt; i++)
                prfs.emplace_back(bucket_size);

            // edges: keep as std::vector (omatcher requires it)
            std::vector<BiEdge<KeyType>> edges;
            edges.reserve(n * prf_cnt);
            KeyType dummy_ctr = 0;
            for (KeyType i = 0; i < n; i++)
            {
                for (KeyType j = 0; j < prf_cnt; j++)
                {
                    KeyType dest_id = prfs[j](data[i].dummy() ? --dummy_ctr : data[i].id) + j * bucket_size;
                    edges.emplace_back(i, dest_id);
                }
            }
            std::vector<KeyType> matchL = omatcher(edges, n, (KeyType)prf_cnt);

            // The deterministic planner bounds cuckoo n to MAJOR_BIN_SIZE, so
            // this sorting workspace is at most about 4.5 MiB and must remain
            // local: ExtVector references are invalidated by cache eviction.
            KeyType tmp_size = 3 * n;
            auto tmp = std::make_unique<PairVec_t>(tmp_size, PairBlock_t());
            Block_t dummy_block;
            KeyType tmp_idx = 0;
            for (KeyType i = 0; i < n; i++)
                (*tmp)[tmp_idx++] = PairBlock_t(matchL[i], data[i]);
            for (KeyType i = 0; i < 2 * n; i++)
                (*tmp)[tmp_idx++] = PairBlock_t(i, dummy_block);

            osorter(tmp->begin(), tmp_size, [](const auto &a, const auto &b)
                    {
                        bool cond1 = a.first != b.first;
                        bool ret1 = a.first < b.first;
                        bool cond2 = a.second.dummy() != b.second.dummy();
                        bool ret2 = !a.second.dummy();
                        bool ret3 = a.second.id < b.second.id;
                        return (cond1 & ret1) | (!cond1 & ((cond2 & ret2) | (!cond2 & ret3))); });
            KeyType n_2 = 2 * n;
            for (KeyType i = 1; i < tmp_size; i++)
                CMOV((*tmp)[i - 1].first == (*tmp)[i].first, (*tmp)[i].first, n_2);
            osorter(tmp->begin(), tmp_size, [](const auto &a, const auto &b)
                    {
                        bool cond1 = a.first != b.first;
                        bool ret1 = a.first < b.first;
                        bool cond2 = a.second.dummy() != b.second.dummy();
                        bool ret2 = !a.second.dummy();
                        bool ret3 = a.second.id < b.second.id;
                        return (cond1 & ret1) | (!cond1 & ((cond2 & ret2) | (!cond2 & ret3))); });

            // Pre-allocate the logical cuckoo table and write its entries.
            entries_capacity = 2 * n;
            entries = std::make_unique<ExtVec_t>(entries_capacity, Block_t());
            for (KeyType i = 0; i < entries_capacity; i++)
                (*entries)[i] = (*tmp)[i].second;

#if H2O2RAM_DIAGNOSTICS
            // Diagnostic-only postcondition: every real input must occupy one
            // of the same PRF slots consulted by operator[].
            for (KeyType i = 0; i < n && n <= 4096; ++i)
            {
                if (data[i].dummy())
                    continue;
                bool found = false;
                bool id_found = false;
                uint64_t observed_value = 0;
                for (uint32_t j = 0; j < prf_cnt; ++j)
                {
                    const KeyType slot = prfs[j](data[i].id) + j * bucket_size;
                    const Block_t candidate = entries->Get(slot);
                    if (candidate.id == data[i].id)
                    {
                        id_found = true;
                        std::memcpy(&observed_value, candidate.value,
                                    std::min(sizeof(observed_value), sizeof(candidate.value)));
                        found |= candidate == data[i];
                    }
                }
                if (!found)
                {
                    uint64_t expected_value = 0;
                    std::memcpy(&expected_value, data[i].value,
                                std::min(sizeof(expected_value), sizeof(data[i].value)));
                    printf("[H2O2RAM ERROR] cuckoo build mismatch key=%llu n=%llu choices=%u "
                           "id_found=%u expected_value=%llu observed_value=%llu\n",
                           static_cast<unsigned long long>(data[i].id),
                           static_cast<unsigned long long>(n),
                           static_cast<unsigned int>(prf_cnt),
                           static_cast<unsigned int>(id_found),
                           static_cast<unsigned long long>(expected_value),
                           static_cast<unsigned long long>(observed_value));
                    std::abort();
                }
            }
#endif
        }

        virtual Block_t operator[](KeyType i)
        {
            Block_t ret;
            for (uint32_t j = 0; j < prf_cnt; j++)
            {
                auto &_ = (*entries)[prfs[j](i) + j * bucket_size];
                CMOV(_.id == i, ret, _);
                CMOV(_.id == i, _.id,
                     _.id | (KeyType(1) << (8 * sizeof(KeyType) - 1)));
            }
#if H2O2RAM_DIAGNOSTICS
            if (i != KeyType(-1) && ret.dummy() && n <= 4096)
            {
                const KeyType dummy_mask =
                    KeyType(1) << (8 * sizeof(KeyType) - 1);
                bool exact_present = false;
                bool prematurely_removed = false;
                KeyType observed_id = KeyType(-1);
                KeyType observed_slot = KeyType(-1);
                for (KeyType slot = 0; slot < entries_capacity; ++slot)
                {
                    const Block_t candidate = entries->Get(slot);
                    if (candidate.id == i)
                    {
                        exact_present = true;
                        observed_id = candidate.id;
                        observed_slot = slot;
                    }
                    if ((candidate.id & ~dummy_mask) == i)
                    {
                        prematurely_removed = true;
                        observed_id = candidate.id;
                        observed_slot = slot;
                    }
                }
                printf("[H2O2RAM ERROR] cuckoo lookup miss key=%llu n=%llu "
                       "exact_present=%u prematurely_removed=%u observed_id=%llu slot=%llu\n",
                       static_cast<unsigned long long>(i),
                       static_cast<unsigned long long>(n),
                       static_cast<unsigned int>(exact_present),
                       static_cast<unsigned int>(prematurely_removed),
                       static_cast<unsigned long long>(observed_id),
                       static_cast<unsigned long long>(observed_slot));
                printf("[H2O2RAM ERROR] candidate_slots=");
                for (uint32_t j = 0; j < prf_cnt; ++j)
                {
                    const KeyType slot = prfs[j](i) + j * bucket_size;
                    const Block_t candidate = entries->Get(slot);
                    printf("%s%llu:%llu", j == 0 ? "" : ",",
                           static_cast<unsigned long long>(slot),
                           static_cast<unsigned long long>(candidate.id));
                }
                printf("\n");
                std::abort();
            }
#endif
            return ret;
        }

        virtual ExtVec_t &data()
        {
            return *entries;
        }

        virtual ExtVec_t &extract()
        {
            // Extract must preserve every live block exactly.  The ported
            // ocompact_by_half path violated that postcondition for full
            // cuckoo levels, so use a fixed real-first sorting network over a
            // stable local workspace; table backing storage remains external.
            std::vector<Block_t> local(entries_capacity);
            for (KeyType i = 0; i < entries_capacity; i++)
                local[i] = (*entries)[i];
            stateless_osorter(local.data(), local.size(),
                              [](const Block_t &a, const Block_t &b)
                              {
                                  const bool different_dummy =
                                      a.dummy() != b.dummy();
                                  const bool real_first = !a.dummy();
                                  const bool id_order = a.id < b.id;
                                  return (different_dummy & real_first) |
                                         (!different_dummy & id_order);
                              });
            KeyType new_cap = entries_capacity / 2;
            auto new_entries = std::make_unique<ExtVec_t>(new_cap, Block_t());
            for (KeyType i = 0; i < new_cap; i++)
                (*new_entries)[i] = local[i];
            entries = std::move(new_entries);
            entries_capacity = new_cap;
            return *entries;
        }

        virtual OHashBase<KeyType, BlockSize> *clone()
        {
            auto ret = new OCuckooHash<KeyType, BlockSize>(this->n, this->delta_inv_log2);
            ret->entries_capacity = this->entries_capacity;
            ret->prfs = this->prfs;
            ret->bucket_size = this->bucket_size;
            ret->entries = std::make_unique<ExtVec_t>(entries_capacity, Block_t());
            for (KeyType i = 0; i < entries_capacity; i++)
                (*ret->entries)[i] = (*this->entries)[i];
            return ret;
        }

        virtual void prepare_synthetic_occupied()
        {
            prfs.clear();
            bucket_size = 2 * n / prf_cnt;
            for (uint32_t i = 0; i < prf_cnt; ++i)
                prfs.emplace_back(bucket_size);
            entries_capacity = 2 * n;
            entries = std::make_unique<ExtVec_t>(
                entries_capacity, Block_t(), EM::ExtVector::LazyInit);
        }

        virtual void prepare_synthetic_extracted()
        {
            // A real extract() compacts a 2n cuckoo table into an n-entry
            // dummy/real stream retained by the now-empty ORAM level.
            prfs.clear();
            bucket_size = 2 * n / prf_cnt;
            for (uint32_t i = 0; i < prf_cnt; ++i)
                prfs.emplace_back(bucket_size);
            entries_capacity = n;
            entries = std::make_unique<ExtVec_t>(
                entries_capacity, Block_t(), EM::ExtVector::LazyInit);
        }

        virtual void reset_synthetic_query_state() {}

        virtual void flush_cache(bool invalidate)
        {
            if (entries)
                entries->FlushCache(invalidate);
        }
    };

} // namespace h2o2ram_ns
