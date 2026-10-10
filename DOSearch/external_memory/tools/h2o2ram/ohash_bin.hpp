#pragma once

#include <type_traits>
#include "ohash_base.hpp"
#include "olinear_scan.hpp"
#include "oshuffle.hpp"

#include "timer.hpp"

namespace h2o2ram_ns {

    template <typename KeyType, std::size_t BlockSize>
    OHashBase<KeyType, BlockSize> *determine_hash(
        KeyType n, KeyType op_num, KeyType delta_inv_log2);

    template <typename KeyType,
              std::size_t BlockSize = sizeof(KeyType)>
    class ObliviousBin
    {
    private:
        using ExtVec_t = typename OHashBase<KeyType, BlockSize>::ExtVec_t;
        KeyType n = 0;
        OHashBase<KeyType, BlockSize> *hash = nullptr;
        bool _empty;

    public:
        ObliviousBin(KeyType n = 1, KeyType op_num = 1,
                     KeyType delta_inv_log2 = DELTA_INV_LOG2)
            : n(n),
              hash(determine_hash<KeyType, BlockSize>(n, op_num,
                                                      delta_inv_log2)),
              _empty(true)
        {
        }

        // copy constructor makes a deep copy
        ObliviousBin(const ObliviousBin &obin) : n(obin.n), _empty(obin._empty)
        {
            if (obin.hash != nullptr)
                hash = obin.hash->clone();
            else
                hash = nullptr;
        }

        // operator= makes a deep copy
        ObliviousBin &operator=(const ObliviousBin &obin)
        {
            if (this != &obin)
            {
                n = obin.n;
                delete hash;
                if (obin.hash != nullptr)
                    hash = obin.hash->clone();
                else
                    hash = nullptr;
                _empty = obin._empty;
            }
            return *this;
        }

        ~ObliviousBin()
        {
            delete hash;
        }

        inline void build(Block<KeyType, BlockSize> *data)
        {
            _empty = false;
            hash->build(data);
        }

        inline Block<KeyType, BlockSize> operator[](KeyType i)
        {
            return hash->operator[](i);
        }

        inline ExtVec_t &data()
        {
            return hash->data();
        }

        inline ExtVec_t &extract()
        {
            _empty = true;
            auto &ret = hash->extract();
            return ret;
        }

        inline bool empty() const
        {
            return _empty;
        }

        inline KeyType capacity() const
        {
            return n;
        }

        inline bool is_linear_scan() const
        {
            return dynamic_cast<OLinearScan<KeyType, BlockSize> *>(hash) != nullptr;
        }

        inline void prepare_synthetic_occupied()
        {
            _empty = false;
            hash->prepare_synthetic_occupied();
        }

        inline void prepare_synthetic_extracted()
        {
            _empty = true;
            hash->prepare_synthetic_extracted();
        }

        inline void reset_synthetic_query_state()
        {
            hash->reset_synthetic_query_state();
        }

        inline void flush_cache(bool invalidate)
        {
            hash->flush_cache(invalidate);
        }

        inline void mark_synthetic_empty()
        {
            _empty = true;
            hash->reset_synthetic_query_state();
        }

        inline void mark_synthetic_occupied()
        {
            _empty = false;
            hash->reset_synthetic_query_state();
        }
    };

} // namespace h2o2ram_ns
