#pragma once

#include <stdint.h>
#include <type_traits>
#include <memory>
#include "oshuffle.hpp"
#include "external_memory/extemvector.hpp"

namespace h2o2ram_ns {

    // H2O2RAM can instantiate thousands of vectors in its two-tier layout.
    // A per-vector 32 MiB default cache would exhaust EPC; three 4 KiB pages
    // retain locality without making cache memory scale with the bin count.
    // Version counters prevent authenticated-page replay and GCM nonce reuse.
    template <typename T>
    using H2OExternalVector = EM::ExtVector::Vector<
        T, std::max((1UL << 12) - 32, sizeof(T)), true, true, 4, true,
        true>;

    template <typename KeyType,
              std::size_t BlockSize = sizeof(KeyType)>
    class OHashBase
    {
    public:
        using Block_t = Block<KeyType, BlockSize>;
        using ExtVec_t = H2OExternalVector<Block_t>;

        virtual ~OHashBase() = default;

        virtual void build(Block_t *data) = 0;
        virtual Block_t operator[](KeyType key) = 0;
        virtual ExtVec_t &data() = 0;
        virtual ExtVec_t &extract() = 0;
        virtual OHashBase<KeyType, BlockSize> *clone() = 0;

        // Synthetic scalability state: allocate the same logical hash
        // topology as a real build/extract, but populate it with dummy pages.
        virtual void prepare_synthetic_occupied() = 0;
        virtual void prepare_synthetic_extracted() = 0;
        virtual void reset_synthetic_query_state() = 0;
        virtual void flush_cache(bool invalidate) = 0;
    };

} // namespace h2o2ram_ns
