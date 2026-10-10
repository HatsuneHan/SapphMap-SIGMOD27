#pragma once

#include "ocuckoo_hash.hpp"
#include "ohash_tiers.hpp"
#include "olinear_scan.hpp"

namespace h2o2ram_ns {

/**
 * Deterministic SGX replacement for upstream's run-time autotuner.
 *
 * The upstream planner benchmarks candidates and persists a host-side config
 * file.  That is neither reproducible nor safe to perform inside the enclave.
 * These cutovers use H2O2RAM's own structural constants: the first bin must be
 * a linear scan, cuckoo construction is bounded to a major-bin-sized input,
 * and larger bins are decomposed by the two-tier hash.
 */
template <typename KeyType, std::size_t BlockSize>
OHashBase<KeyType, BlockSize> *determine_hash(KeyType n,
                                              KeyType /*op_num*/,
                                              KeyType delta_inv_log2)
{
    static_assert(std::is_integral_v<KeyType> && std::is_unsigned_v<KeyType>,
                  "H2O2RAM keys must be unsigned integers");
    if (n == 0)
        return nullptr;
    if (n <= static_cast<KeyType>(SMALL_HASH_TABLE_THRESHOLD))
        return new OLinearScan<KeyType, BlockSize>(n);
    if (n <= static_cast<KeyType>(MAJOR_BIN_SIZE))
        return new OCuckooHash<KeyType, BlockSize>(n, delta_inv_log2);
    return new OTwoTierHash<KeyType, BlockSize>(n, delta_inv_log2,
                                                EPSILON_INV);
}

} // namespace h2o2ram_ns
