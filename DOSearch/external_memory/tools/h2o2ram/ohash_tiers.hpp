#pragma once
#include <type_traits>
#include <cassert>
#include <algorithm>
#include <iterator>
#include <numeric>
#include <random>
#include <stdexcept>
#include <thread>
#include <variant>
#include <vector>
#include "ohash_bin.hpp"
#include "prf.hpp"

#include "timer.hpp"
#ifdef H2O2RAM_DEBUG
#include <fstream>
#endif

#include "ocompact.hpp"

namespace h2o2ram_ns {

    template <typename KeyType,
              std::size_t BlockSize = sizeof(KeyType)>
    class OTwoTierHash : public OHashBase<KeyType, BlockSize>
    {
    private:
        using ExtVec_t = typename OHashBase<KeyType, BlockSize>::ExtVec_t;
        using Block_t = Block<KeyType, BlockSize>;
        using PairBlock_t = PairBlock<KeyType, BlockSize>;
        using PairVec_t = std::vector<PairBlock_t>;

        const KeyType n;
        KeyType dummy_access_ctr = 0;
        bool _empty;

        KeyType bin_size;
        KeyType delta_inv_log2;
        KeyType epsilon_inv;
        const KeyType bin_num;

        // Route on the complete logical key.  Truncating a 64-bit key here
        // groups all keys with the same low 32 bits into the same major bin.
        AESPRF<KeyType> prf;
        std::vector<uint32_t> bin_loads;
        std::vector<ObliviousBin<KeyType, BlockSize>> major_bins;
        ObliviousBin<KeyType, BlockSize> overflow_bin;
        std::mt19937 gen;
        std::unique_ptr<ExtVec_t> extracted_data;
        std::vector<uint32_t> sample_secrete_loads(KeyType n_)
        {
            std::vector<uint32_t> secrete_loads(this->bin_num);
            for (KeyType i = 0; i < this->bin_num - 1; i++)
            {
                std::binomial_distribution<KeyType> dist(n_, 1.0 / (this->bin_num - i));
                secrete_loads[i] = dist(this->gen);
                n_ -= secrete_loads[i];
            }
            secrete_loads[this->bin_num - 1] = n_;
            return secrete_loads;
        }

        static inline KeyType compute_bin_size(const KeyType epsilon_inv)
        {
            return epsilon_inv * epsilon_inv * 1024;
        }

    public:
        static std::tuple<KeyType, double> compute_epsilon_inv(
            Block<KeyType, BlockSize> *data,
            const KeyType n,
            const KeyType delta_inv_log2 = DELTA_INV_LOG2)
        {
            std::mt19937 gen(H2O2RAM::secure_seed());
            std::uniform_int_distribution<uint32_t> dist(0, n - 1);
            KeyType epsilon_inv = 2;
            auto bin_size = compute_bin_size(epsilon_inv);
            double min_time = std::numeric_limits<double>::max();
            KeyType ret_eps_inv = 2;
            DepthCounter _;
            while (bin_size < n)
            {
                for (KeyType i = 0; i < n; i++)
                    data[i].id = i;
                std::shuffle(data, data + n, gen);
                OTwoTierHash<KeyType, BlockSize> oht(n, delta_inv_log2, epsilon_inv);
                Timer t;
                oht.build(data);
                for (KeyType i = 0; i < n; i++)
                    oht[dist(gen)];
                oht.extract();
                double cur_time = t.get_total_time();
                if (cur_time < min_time)
                {
                    min_time = cur_time;
                    ret_eps_inv = epsilon_inv;
                }
                epsilon_inv <<= 1;
                bin_size = compute_bin_size(epsilon_inv);
#ifdef H2O2RAM_DEBUG
                std::cout << _ << "epsilon_inv: " << epsilon_inv << ", bin_size: " << bin_size << ", time: " << cur_time << std::endl;
#endif
            }
            return {ret_eps_inv, min_time};
        }

        OTwoTierHash(const KeyType n,
                     const KeyType delta_inv_log2 = DELTA_INV_LOG2,
                     const KeyType epsilon_inv = EPSILON_INV)
            : n(n),
              _empty(true),
              bin_size(compute_bin_size(epsilon_inv)),
              delta_inv_log2(delta_inv_log2),
              epsilon_inv(epsilon_inv),
              bin_num(n > bin_size ? KeyType(2) * n / bin_size : KeyType(1)),
              prf(bin_num),
              bin_loads(bin_num),
              major_bins(bin_num,
                         ObliviousBin<KeyType,
                                      BlockSize>(bin_num == 1 ? n : bin_size / 2,
                                                 n / bin_num,
                                                 delta_inv_log2)),
              overflow_bin(bin_num > 1 ? n / epsilon_inv : 0,
                           bin_num > 1 ? n : 0,
                           delta_inv_log2),
              gen(H2O2RAM::secure_seed())
        {
            assert((((n) & ((n) - 1)) == 0));
        }

        virtual void build(Block<KeyType, BlockSize> *data)
        {
            dummy_access_ctr = 0;
            _empty = false;
            extracted_data.reset();
            if (this->bin_num == 1)
            {
                bin_loads[0] = n;
                major_bins[0].build(data);
                return;
            }

            std::mt19937 shuffle_gen(H2O2RAM::secure_seed());
            // The Fisher-Yates access pattern depends only on fresh enclave
            // randomness, not on block contents.  It matches upstream and
            // avoids an n-element key array plus an unsupported external sort.
            std::shuffle(data, data + n, shuffle_gen);
            this->prf.reset();
            std::uniform_int_distribution<uint32_t> dist(0, this->bin_num - 1);

            auto buffer = std::make_unique<ExtVec_t>(KeyType(2) * n, Block_t());
            std::fill(bin_loads.begin(), bin_loads.end(), 0);
            for (KeyType i = 0; i < this->n; i++)
            {
                KeyType bin_id = prf(data[i].id);
                CMOV(data[i].dummy(), bin_id, (KeyType)dist(this->gen));
                bin_loads[bin_id]++;
                (*buffer)[bin_id * bin_size + bin_loads[bin_id] - 1] = data[i];
            }
            std::vector<KeyType> indices(bin_num);
            std::iota(indices.begin(), indices.end(), 0);
            std::vector<uint32_t> secrete_loads = sample_secrete_loads(n - n / epsilon_inv);

            KeyType overflow_cap = n / epsilon_inv * 2;
            // This workspace is n/4 blocks (4 GiB at the supported 2^30
            // ceiling).  Keeping it local makes ocompact's pointer arithmetic
            // valid and keeps the analytical enclave-heap peak below 32 GiB;
            // table storage remains external.
            std::vector<Block_t> overflow_data(overflow_cap);
            for (KeyType i = 0; i < indices.size(); i++)
            {
                KeyType idx = indices[i];
                KeyType bin_st = bin_size * idx;
                KeyType overflow_st = (bin_size / epsilon_inv) * idx;
                auto tmp = bin_st + KeyType(bin_loads[idx] - bin_size / epsilon_inv);
                for (KeyType j = tmp; j < bin_st + (KeyType)bin_loads[idx]; j++)
                {
                    obliSwap((*buffer)[j],
                             overflow_data[overflow_st + j - tmp],
                             j >= bin_st + secrete_loads[idx]);
                }
            }
            for (KeyType i = 0; i < indices.size(); i++)
            {
                KeyType idx = indices[i];
                KeyType bin_st = bin_size * idx;
                std::vector<Block_t> bin_data(bin_size);
                for (KeyType j = 0; j < bin_size; j++)
                    bin_data[j] = (*buffer)[bin_st + j];
                major_bins[idx].build(bin_data.data());
            }

            // Fixed real-first ordering gives the overflow half an exact
            // preservation postcondition; this is the same access sequence
            // for every input of this public size.
            stateless_osorter(overflow_data.data(), overflow_data.size(),
                              [](const Block_t &a, const Block_t &b)
                              {
                                  const bool different_dummy =
                                      a.dummy() != b.dummy();
                                  const bool real_first = !a.dummy();
                                  const bool id_order = a.id < b.id;
                                  return (different_dummy & real_first) |
                                         (!different_dummy & id_order);
                              });
            KeyType overflow_real = overflow_cap / 2;
            std::vector<Block_t> overflow_local(overflow_real);
            for (KeyType i = 0; i < overflow_real; i++)
                overflow_local[i] = overflow_data[i];
            overflow_bin.build(overflow_local.data());
        }

        virtual Block<KeyType, BlockSize> operator[](KeyType key)
        {
            CMOV(key == KeyType(-1), key, --dummy_access_ctr);
            if (bin_num > 1)
            {
                auto ret = overflow_bin[key];
                if (ret.dummy())
                {
                    KeyType bin_id = prf(key);
                    ret = major_bins[bin_id][key];
                }
                else
                {
                    --dummy_access_ctr;
                    KeyType bin_id = prf(dummy_access_ctr);
                    major_bins[bin_id][dummy_access_ctr];
                }
                return ret;
            }
            else
                return major_bins[0][key];
        }

        virtual ExtVec_t &data()
        {
            assert(extracted_data && "data() not supported on OTwoTierHash");
            return *extracted_data;
        }

        virtual ExtVec_t &extract()
        {
            _empty = true;
            auto sort_cmp = [](const auto &a, const auto &b)
            {
                bool cond1 = a.dummy() != b.dummy();
                bool ret1 = !a.dummy();
                bool ret2 = a.id < b.id;
                return (cond1 & ret1) | (!cond1 & ret2);
            };

            if (bin_num == 1)
            {
                auto &bin_data_ref = major_bins[0].extract();
                auto bin_size_val = bin_data_ref.size();
                std::vector<Block_t> local_vec(bin_size_val);
                for (size_t i = 0; i < bin_size_val; i++)
                    local_vec[i] = bin_data_ref[i];
                osorter(local_vec.data(), local_vec.size(), sort_cmp);
                KeyType count = std::accumulate(bin_loads.begin(), bin_loads.end(), 0);
                extracted_data = std::make_unique<ExtVec_t>(count, Block_t());
                for (size_t i = 0; i < count; i++)
                    (*extracted_data)[i] = local_vec[i];
                return *extracted_data;
            }

            auto &overflow_ref = overflow_bin.extract();
            KeyType overflow_sz = overflow_ref.size();
            KeyType group_size = bin_size / epsilon_inv;
            KeyType tmp_size = overflow_sz + bin_num * group_size;
            auto tmp = std::make_unique<PairVec_t>(tmp_size, PairBlock_t());
            KeyType tmp_idx = 0;
            for (size_t i = 0; i < overflow_sz; i++)
            {
                auto e = overflow_ref[i];
                (*tmp)[tmp_idx++] = PairBlock_t(oblivious_select(prf(e.id), bin_num, e.dummy()), e);
            }
            for (KeyType i = 0; i < bin_num; i++)
                for (KeyType j = 0; j < group_size; j++)
                    (*tmp)[tmp_idx++] = PairBlock_t(i, Block_t());

            osorter(tmp->begin(), tmp_size, [](const auto &a, const auto &b)
                    {
                        bool cond1 = a.first != b.first;
                        bool ret1 = a.first < b.first;
                        bool cond2 = a.second.dummy() != b.second.dummy();
                        bool ret2 = !a.second.dummy();
                        bool ret3 = a.second.id < b.second.id;
                        return (cond1 & ret1) | (!cond1 & ((cond2 & ret2) | (!cond2 & ret3))); });
            KeyType cnt = 1;
            KeyType prev_first = (*tmp)[0].first;
            for (size_t i = 1; i < tmp_size; i++)
            {
                ++cnt;
                CMOV((*tmp)[i].first == prev_first, cnt, KeyType(1));
                prev_first = (*tmp)[i].first;
                CMOV(cnt > group_size, (*tmp)[i].first, bin_num);
            }
            osorter(tmp->begin(), tmp_size, [](const auto &a, const auto &b)
                    {
                        bool cond1 = a.first != b.first;
                        bool ret1 = a.first < b.first;
                        bool cond2 = a.second.dummy() != b.second.dummy();
                        bool ret2 = !a.second.dummy();
                        bool ret3 = a.second.id < b.second.id;
                        return (cond1 & ret1) | (!cond1 & ((cond2 & ret2) | (!cond2 & ret3))); });

            KeyType total_real = std::accumulate(bin_loads.begin(), bin_loads.end(), 0);
            extracted_data = std::make_unique<ExtVec_t>(total_real, Block_t());
            KeyType offset = 0;
            for (size_t bin_idx = 0; bin_idx < major_bins.size(); bin_idx++)
            {
                auto &bin = major_bins[bin_idx];
                auto &bin_data_ref = bin.extract();
                auto bin_data_size = bin_data_ref.size();
                auto local_bin_sz = bin_data_size + group_size;
                std::vector<Block_t> local_bin(local_bin_sz);
                for (size_t i = 0; i < bin_data_size; i++)
                    local_bin[i] = bin_data_ref[i];
                for (KeyType j = 0; j < group_size; j++)
                    local_bin[bin_data_size + j] = (*tmp)[bin_idx * group_size + j].second;
                osorter(local_bin.data(), local_bin.size(), sort_cmp);
                for (KeyType j = 0; j < bin_loads[bin_idx]; j++)
                    (*extracted_data)[offset + j] = local_bin[j];
                offset += bin_loads[bin_idx];
            }
            return *extracted_data;
        }

        bool empty() const
        {
            return _empty;
        }

        virtual OHashBase<KeyType, BlockSize> *clone()
        {
            auto ret = new OTwoTierHash<KeyType, BlockSize>(this->n, this->delta_inv_log2, this->epsilon_inv);
            ret->dummy_access_ctr = this->dummy_access_ctr;
            ret->_empty = this->_empty;
            ret->bin_size = this->bin_size;
            ret->delta_inv_log2 = this->delta_inv_log2;
            ret->epsilon_inv = this->epsilon_inv;
            ret->prf = this->prf;
            ret->bin_loads = this->bin_loads;
            ret->major_bins = this->major_bins;
            ret->overflow_bin = this->overflow_bin;
            ret->gen = this->gen;
            if (this->extracted_data)
            {
                auto cap = this->extracted_data->size();
                ret->extracted_data = std::make_unique<ExtVec_t>(cap, Block_t());
                for (size_t i = 0; i < cap; i++)
                    (*ret->extracted_data)[i] = (*this->extracted_data)[i];
            }
            return ret;
        }

        virtual void prepare_synthetic_occupied()
        {
            dummy_access_ctr = 0;
            _empty = false;
            extracted_data.reset();
            prf.reset();
            std::fill(bin_loads.begin(), bin_loads.end(),
                      static_cast<uint32_t>(n / bin_num));
            for (auto& bin : major_bins)
                bin.prepare_synthetic_occupied();
            if (bin_num > 1)
                overflow_bin.prepare_synthetic_occupied();
        }

        virtual void prepare_synthetic_extracted()
        {
            // Match the retained state produced by extract(): the outer n
            // stream and recursively extracted child bins remain allocated.
            dummy_access_ctr = 0;
            _empty = true;
            for (auto& bin : major_bins)
                bin.prepare_synthetic_extracted();
            if (bin_num > 1)
                overflow_bin.prepare_synthetic_extracted();
            extracted_data = std::make_unique<ExtVec_t>(
                n, Block_t(), EM::ExtVector::LazyInit);
        }

        virtual void reset_synthetic_query_state()
        {
            dummy_access_ctr = 0;
            for (auto& bin : major_bins)
                bin.reset_synthetic_query_state();
            if (bin_num > 1)
                overflow_bin.reset_synthetic_query_state();
        }

        virtual void flush_cache(bool invalidate)
        {
            for (auto& bin : major_bins)
                bin.flush_cache(invalidate);
            if (bin_num > 1)
                overflow_bin.flush_cache(invalidate);
            if (extracted_data)
                extracted_data->FlushCache(invalidate);
        }
    };

} // namespace h2o2ram_ns
