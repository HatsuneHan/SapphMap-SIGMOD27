#pragma once
#include <type_traits>
#include "oblivious_operations.hpp"

namespace h2o2ram_ns {

    template <typename DataIt, typename Comparator>
    static void bitonic_merge(DataIt data,
                              uint64_t low,
                              uint64_t cnt,
                              bool dir,
                              Comparator cmp)
    {
        if (cnt <= 1)
            return;
        uint64_t k = 1;
        while (k < cnt)
            k <<= 1;
        k >>= 1;

        for (uint64_t i = low; i < low + cnt - k; i++)
            obliSwap(data[i], data[i + k],
                     dir == !cmp(data[i], data[i + k]));
        bitonic_merge(data, low, k, dir, cmp);
        bitonic_merge(data, low + k, cnt - k, dir, cmp);
    }

    template <typename DataIt, typename Comparator = std::less<typename std::iterator_traits<DataIt>::value_type>>
    void bitonic_sort(DataIt data,
                      uint64_t low,
                      uint64_t cnt,
                      bool dir,
                      Comparator cmp = Comparator())
    {
        if (cnt <= 1)
            return;
        const uint64_t k = cnt >> 1;
        bitonic_sort(data, low, k, !dir, cmp);
        bitonic_sort(data, low + k, cnt - k, dir, cmp);
        bitonic_merge(data, low, cnt, dir, cmp);
    }

    template <typename DataIt, typename Comparator = std::less<typename std::iterator_traits<DataIt>::value_type>>
    void stateless_osorter(DataIt data, uint64_t n, Comparator cmp = Comparator())
    {
        bitonic_sort(data, 0, n, true, cmp);
    }

} // namespace h2o2ram_ns
