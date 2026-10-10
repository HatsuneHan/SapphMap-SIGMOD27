#pragma once

#include "ohash_base.hpp"
#include "types.hpp"

namespace h2o2ram_ns {

    template <typename KeyType,
              std::size_t BlockSize = sizeof(KeyType)>
    class OLinearScan : public OHashBase<KeyType, BlockSize>
    {
    private:
        using Block_t = Block<KeyType, BlockSize>;
        using ExtVec_t = typename OHashBase<KeyType, BlockSize>::ExtVec_t;
        std::unique_ptr<ExtVec_t> entries;
        KeyType n;

    public:
        OLinearScan(KeyType n) : entries(std::make_unique<ExtVec_t>(n, Block_t())), n(n) {}

        virtual void build(Block_t *data)
        {
            for (KeyType i = 0; i < n; i++)
                (*entries)[i] = data[i];
        }

        inline virtual Block_t operator[](KeyType i)
        {
            Block_t ret;
            for (KeyType j = 0; j < n; j++)
            {
                auto &_ = (*entries)[j];
                CMOV(_.id == i, ret, _);
                CMOV(_.id == i, _.id, _.id | (KeyType(1) << (8 * sizeof(KeyType) - 1)));
            }
            return ret;
        }

        virtual ExtVec_t &data()
        {
            return *entries;
        }

        virtual ExtVec_t &extract()
        {
            return *entries;
        }

        virtual OHashBase<KeyType, BlockSize> *clone()
        {
            auto ret = new OLinearScan<KeyType, BlockSize>(this->n);
            for (KeyType i = 0; i < n; i++)
                (*ret->entries)[i] = (*this->entries)[i];
            return ret;
        }

        virtual void prepare_synthetic_occupied()
        {
            entries = std::make_unique<ExtVec_t>(
                n, Block_t(), EM::ExtVector::LazyInit);
        }

        virtual void prepare_synthetic_extracted()
        {
            prepare_synthetic_occupied();
        }

        virtual void reset_synthetic_query_state() {}

        virtual void flush_cache(bool invalidate)
        {
            if (entries)
                entries->FlushCache(invalidate);
        }
    };

} // namespace h2o2ram_ns
