#pragma once

#include <assert.h>
#include <algorithm>
#include <iterator>
#include <numeric>
#include <random>
#include <stdexcept>
#include <thread>
#include <variant>
#include <vector>
#include "iterator_stride.hpp"
#include "h2o2ram_aes.hpp"

#ifdef H2O2RAM_DEBUG
#include <iostream>
#include <iomanip>
#endif
#include "depth_counter.hpp"

namespace h2o2ram_ns {

    // nonoblivious optimal compaction
    template <typename DataIt, typename FlagIt>
    void compact(DataIt data_first, DataIt data_last, FlagIt flag_first)
    {
        std::partition(data_first, data_last, [&flag_first, &data_first](const auto &item)
                       { return *(flag_first + (&item - &*data_first)); });
    }

    // nonoblivious compaction based on sort, only for testing, 0 first
    template <typename DataIt, typename FlagIt>
    void compact_by_sort(DataIt data_first, DataIt data_last, FlagIt flag_first)
    {
        std::vector<size_t> indices(data_last - data_first);
        std::iota(indices.begin(), indices.end(), 0);

        std::sort(indices.begin(), indices.end(),
                  [&flag_first](size_t i, size_t j)
                  {
                      return flag_first[i] < flag_first[j];
                  });
        for (size_t i = 0; i < indices.size(); i++)
        {
            while (indices[i] != i)
            {
                std::swap(data_first[indices[indices[i]]], data_first[indices[i]]);
                std::swap(flag_first[indices[indices[i]]], flag_first[indices[i]]);
                std::swap(indices[i], indices[indices[i]]);
            }
        }
    }

    template <typename DataIt,
              typename FlagIt,
              typename OffsetsIt>
    void _or_off_compact_serial(DataIt data_first,
                                FlagIt flag_first,
                                OffsetsIt offsets_first,
                                const typename std::iterator_traits<OffsetsIt>::value_type start_index,
                                const typename std::iterator_traits<OffsetsIt>::value_type z,
                                const typename std::iterator_traits<OffsetsIt>::value_type n)
    {
        using offset_type = typename std::iterator_traits<OffsetsIt>::value_type;
        if (n <= 1)
            return;
        if (n == 2)
        {
            offset_type p1 = 1 + (start_index == 0 ? 0 : offsets_first[start_index - 1]) - offsets_first[start_index];
            offset_type p2 = offsets_first[start_index + 1] - offsets_first[start_index];
            obliSwap(data_first[start_index], data_first[start_index + 1],
                           (p1 & p2) ^ z);
            obliSwap(flag_first[start_index], flag_first[start_index + 1],
                           (p1 & p2) ^ z);
            return;
        }
        const offset_type mod = n / 2 - 1;
        const offset_type m = offsets_first[start_index + mod] - (start_index == 0 ? 0
                                                                                   : offsets_first[start_index - 1]);
        _or_off_compact_serial(data_first, flag_first,
                               offsets_first,
                               start_index, z & mod,
                               n / 2);
        _or_off_compact_serial(data_first, flag_first,
                               offsets_first,
                               start_index + n / 2, (z + m) & mod,
                               n / 2);
        const offset_type
            s = (((z & mod) + m) >= n / 2) ^ (z >= n / 2);
        const auto data_st = data_first + start_index;
        const auto flag_st = flag_first + start_index;
        std::for_each(data_st,
                      data_st + n / 2,
                      [z, m, s, data_st, n, mod](auto &x)
                      {
                          auto cur_iter = data_st + (&x - &*data_st);
                          if constexpr (std::is_same<decltype(cur_iter),
                                                     IteratorStride<typename std::remove_reference<decltype(x)>::type>>::value)
                          {
                              cur_iter.set_stride(data_st.get_stride());
                          }
                          offset_type i = cur_iter - data_st;
                          obliSwap(x,
                                         *(cur_iter + n / 2),
                                         (i >= ((z + m) & mod)) ^ s);
                      });
        std::for_each(flag_st,
                      flag_st + n / 2,
                      [z, m, s, flag_st, n, mod](auto &x)
                      {
                          auto cur_iter = flag_st + (&x - &*flag_st);
                          if constexpr (std::is_same<decltype(cur_iter),
                                                     IteratorStride<typename std::remove_reference<decltype(x)>::type>>::value)
                              cur_iter.set_stride(flag_st.get_stride());
                          offset_type i = cur_iter - flag_st;
                          obliSwap(x,
                                         *(cur_iter + n / 2),
                                         (i >= ((z + m) & mod)) ^ s);
                      });
    }

    template <typename DataIt,
              typename FlagIt,
              typename OffsetsIt>
    void _or_off_compact(DataIt data_first,
                         FlagIt flag_first,
                         OffsetsIt offsets_first,
                         const typename std::iterator_traits<OffsetsIt>::value_type start_index,
                         const typename std::iterator_traits<OffsetsIt>::value_type z,
                         const typename std::iterator_traits<OffsetsIt>::value_type n,
                         int threads)
    {
        using offset_type = typename std::iterator_traits<OffsetsIt>::value_type;
        if (n <= 1)
            return;
        if (n == 2)
        {
            offset_type p1 = 1 + (start_index == 0 ? 0 : offsets_first[start_index - 1]) - offsets_first[start_index];
            offset_type p2 = offsets_first[start_index + 1] - offsets_first[start_index];
            obliSwap(data_first[start_index], data_first[start_index + 1],
                           (p1 & p2) ^ z);
            obliSwap(flag_first[start_index], flag_first[start_index + 1],
                           (p1 & p2) ^ z);
            return;
        }
        const offset_type mod = n / 2 - 1;
        const offset_type m = offsets_first[start_index + mod] - (start_index == 0 ? 0
                                                                                   : offsets_first[start_index - 1]);

        if (threads <= 1 || n <= (1 << 12))
        {
            _or_off_compact_serial(data_first, flag_first, offsets_first, start_index, z, n);
            return;
        }
        else
        {
            _or_off_compact(data_first, flag_first,
                            offsets_first,
                            start_index, z & mod,
                            n / 2,
                            threads / 2);
            _or_off_compact(data_first, flag_first,
                            offsets_first,
                            start_index + n / 2, (z + m) & mod,
                            n / 2,
                            threads / 2);
        }
        const offset_type
            s = (((z & mod) + m) >= n / 2) ^ (z >= n / 2);
        const auto data_st = data_first + start_index;
        const auto flag_st = flag_first + start_index;
        std::for_each(data_st,
                      data_st + n / 2,
                      [z, m, s, data_st, n, mod](auto &x)
                      {
                          auto cur_iter = data_st + (&x - &*data_st);
                          if constexpr (std::is_same<decltype(cur_iter),
                                                     IteratorStride<typename std::remove_reference<decltype(x)>::type>>::value)
                          {
                              cur_iter.set_stride(data_st.get_stride());
                          }
                          offset_type i = cur_iter - data_st;
                          obliSwap(x,
                                         *(cur_iter + n / 2),
                                         (i >= ((z + m) & mod)) ^ s);
                      });
        std::for_each(flag_st,
                      flag_st + n / 2,
                      [z, m, s, flag_st, n, mod](auto &x)
                      {
                          auto cur_iter = flag_st + (&x - &*flag_st);
                          if constexpr (std::is_same<decltype(cur_iter),
                                                     IteratorStride<typename std::remove_reference<decltype(x)>::type>>::value)
                              cur_iter.set_stride(flag_st.get_stride());
                          offset_type i = cur_iter - flag_st;
                          obliSwap(x,
                                         *(cur_iter + n / 2),
                                         (i >= ((z + m) & mod)) ^ s);
                      });
    }

    template <typename DataIt,
              typename FlagIt,
              typename PrefixSumTp>
    void _or_off_compact_entry(DataIt data_first,
                               FlagIt flag_first,
                               PrefixSumTp n,
                               int threads = 1)
    {
        auto unary_op = [](typename std::iterator_traits<FlagIt>::value_type x) -> PrefixSumTp
        {
            return x;
        };
        std::vector<PrefixSumTp> offset_prefix_sum(n);
        std::transform_inclusive_scan(flag_first, flag_first + n,
                                      offset_prefix_sum.begin(),
                                      std::plus<PrefixSumTp>{},
                                      unary_op);

        _or_off_compact(data_first, flag_first, offset_prefix_sum.begin(), 0, 0, n, threads);
    }

    // 1s first
    template <typename DataIt,
              typename FlagIt>
    void or_compact_power_2(DataIt data_first,
                            FlagIt flag_first,
                            size_t n,
                            size_t threads = 1)
    {
        if (n <= 1)
            return;
        if (n == 2)
        {
            obliSwap(*data_first, *(data_first + 1), !*flag_first);
            obliSwap(*flag_first, *(flag_first + 1), !*flag_first);
            return;
        }
        assert((((n) & ((n) - 1)) == 0));
        if (n <= PARTIAL_SUM_PARALLEL_THRESHOLD)
            _or_off_compact_entry<DataIt, FlagIt, uint32_t>(
                data_first, flag_first, (uint32_t)n, threads);
        else if (n < std::numeric_limits<int32_t>::max())
            _or_off_compact_entry<DataIt, FlagIt, uint32_t>(
                data_first, flag_first, (uint32_t)n, threads);
        else
            _or_off_compact_entry<DataIt, FlagIt, size_t>(
                data_first, flag_first, n, threads);
    }

    template <typename DataIt,
              typename FlagIt,
              typename NumTp,
              typename EngineTp>
    void _ocompact_by_half_rand_cyclic_shift(DataIt data_first,
                                             FlagIt flag_first,
                                             const NumTp n,
                                             const NumTp Z,
                                             EngineTp &gen)
    {
        assert((((Z) & ((Z) - 1)) == 0));
        using DataTp = typename std::iterator_traits<DataIt>::value_type;
        using FlagTp = typename std::iterator_traits<FlagIt>::value_type;
        const NumTp b = n / Z;
        if (b <= 1 || b * sizeof(DataTp) < 32768)
        {
            or_compact_power_2(data_first, flag_first, n);
            return;
        }
        std::uniform_int_distribution<NumTp> dist(0, b - 1);
        std::vector<NumTp> shifts(Z);
        for (NumTp i = 0; i < Z; i++)
            shifts[i] = dist(gen);

        for (NumTp i = 0; i < Z; i++)
        {
            NumTp start = i * b;
            NumTp end = (i + 1) * b;
            std::rotate(data_first + start,
                        data_first + start + shifts[i],
                        data_first + end);
            std::rotate(flag_first + start,
                        flag_first + start + shifts[i],
                        flag_first + end);
        }

        if (sizeof(DataTp) > 32768)
        {
            for (NumTp i = 0; i < b; i++)
            {
                IteratorStride<DataTp> data_stride_iter(data_first + i, b);
                IteratorStride<FlagTp> flag_stride_iter(flag_first + i, b);
                _or_off_compact_entry(
                    data_stride_iter, flag_stride_iter, (uint32_t)Z, 1);
            }
        }
        else
        {
            std::vector<DataTp> data_temp(Z);
            std::vector<FlagTp> flag_temp(Z);
            for (NumTp i = 0; i < b; i++)
            {
                for (NumTp j = 0; j < Z; j++)
                {
                    data_temp[j] = data_first[i + j * b];
                    flag_temp[j] = flag_first[i + j * b];
                }
                _or_off_compact_entry(data_temp.begin(), flag_temp.begin(),
                                      (uint32_t)Z, 1);
                for (NumTp j = 0; j < Z; j++)
                {
                    data_first[i + j * b] = data_temp[j];
                    flag_first[i + j * b] = flag_temp[j];
                }
            }
        }

        // recursion on the middle half
        if constexpr (std::is_same<size_t, decltype(n)>::value)
        {
            if (n / 2 < (size_t)std::numeric_limits<int32_t>::max())
                _ocompact_by_half_rand_cyclic_shift<DataIt, FlagIt,
                                                    uint32_t, EngineTp>(data_first + n / 4,
                                                                        flag_first + n / 4,
                                                                        n / 2,
                                                                        Z,
                                                                        gen);
            else
                _ocompact_by_half_rand_cyclic_shift<DataIt, FlagIt,
                                                    size_t, EngineTp>(data_first + n / 4,
                                                                      flag_first + n / 4,
                                                                      n / 2,
                                                                      Z,
                                                                      gen);
        }
        else
            _ocompact_by_half_rand_cyclic_shift<DataIt, FlagIt,
                                                uint32_t, EngineTp>(data_first + n / 4,
                                                                    flag_first + n / 4,
                                                                    n / 2,
                                                                    Z,
                                                                    gen);
    }

    // 1s first
    template <typename DataIt,
              typename FlagIt>
    void ocompact_by_half(DataIt data_first,
                          FlagIt flag_first,
                          size_t n,
                          size_t Z,
                          uint32_t seed = H2O2RAM::secure_seed())
    {
        static std::mt19937 gen(seed);
        assert((((n) & ((n) - 1)) == 0));
        assert((((Z) & ((Z) - 1)) == 0));
        if (n < (size_t)std::numeric_limits<int32_t>::max())
            _ocompact_by_half_rand_cyclic_shift<DataIt, FlagIt,
                                                uint32_t, std::mt19937>(
                data_first, flag_first, (uint32_t)n, Z, gen);
        else
            _ocompact_by_half_rand_cyclic_shift<DataIt, FlagIt,
                                                size_t, std::mt19937>(
                data_first, flag_first, n, Z, gen);
    }

    template <typename DataIt,
              typename FlagIt,
              typename NumTp>
    void _ocompact_by_half_rand_exp(DataIt data_first,
                                    FlagIt flag_first,
                                    const NumTp n,
                                    const NumTp Z,
                                    const NumTp depth,
                                    const NumTp budget)
    {
        using DataTp = typename std::iterator_traits<DataIt>::value_type;
        using FlagTp = typename std::iterator_traits<FlagIt>::value_type;
        // cyclically shift each bucket
        const NumTp b = n / Z;
        if (b <= 1 || b * sizeof(DataTp) < 32768)
        {
            or_compact_power_2(data_first, flag_first, n);
            return;
        }
        // compact two compacted arrays with totally half distinguisable items
        if (b == 2 && depth != 0)
        {
            for (NumTp i = 0; i < Z; i++)
            {
                obliSwap(data_first[2 * (Z - 1 - i)],
                               data_first[2 * i + 1],
                               (!flag_first[2 * (Z - 1 - i)]) & flag_first[2 * i + 1]);
                obliSwap(flag_first[2 * (Z - 1 - i)],
                               flag_first[2 * i + 1],
                               (!flag_first[2 * (Z - 1 - i)]) & flag_first[2 * i + 1]);
            }
            return;
        }

        if (sizeof(DataTp) > 32768)
        {
            for (NumTp i = 0; i < b; i++)
            {
                IteratorStride<DataTp> data_stride_iter(data_first + i, b);
                IteratorStride<FlagTp> flag_stride_iter(flag_first + i, b);
                _or_off_compact_entry(
                    data_stride_iter, flag_stride_iter, (uint32_t)Z, 1);
            }
        }
        else
        {
            std::vector<DataTp> data_temp(Z);
            std::vector<FlagTp> flag_temp(Z);
            for (NumTp i = 0; i < b; i++)
            {
                for (NumTp j = 0; j < Z; j++)
                {
                    data_temp[j] = data_first[i + j * b];
                    flag_temp[j] = flag_first[i + j * b];
                }
                _or_off_compact_entry(data_temp.begin(), flag_temp.begin(),
                                      (uint32_t)Z, 1);
                for (NumTp j = 0; j < Z; j++)
                {
                    data_first[i + j * b] = data_temp[j];
                    flag_first[i + j * b] = flag_temp[j];
                }
            }
        }

        // recursion on the middle half
        if constexpr (std::is_same<size_t, decltype(n)>::value)
        {
            if (n / 2 < (size_t)std::numeric_limits<int32_t>::max())
                _ocompact_by_half_rand_exp<DataIt, FlagIt,
                                           uint32_t>(data_first + n / 4,
                                                     flag_first + n / 4,
                                                     n / 2,
                                                     Z * 2,
                                                     depth + 1,
                                                     depth % 2 == 0 && budget > 1 ? budget - 1 : budget);
            else
                _ocompact_by_half_rand_exp<DataIt, FlagIt,
                                           size_t>(data_first + n / 4,
                                                   flag_first + n / 4,
                                                   n / 2,
                                                   Z * 2,
                                                   depth + 1,
                                                   depth % 2 == 0 && budget > 1 ? budget - 1 : budget);
        }
        else
            _ocompact_by_half_rand_exp<DataIt, FlagIt,
                                       uint32_t>(data_first + n / 4,
                                                 flag_first + n / 4,
                                                 n / 2,
                                                 Z * 2,
                                                 depth + 1,
                                                 depth % 2 == 0 && budget > 1 ? budget - 1 : budget);
    }

    // 1s first
    template <typename DataIt,
              typename FlagIt>
    void ocompact_by_half_exp(DataIt data_first,
                              FlagIt flag_first,
                              size_t n,
                              size_t Z = OCOMPACT_Z)
    {
        assert((((n) & ((n) - 1)) == 0));
        assert((((Z) & ((Z) - 1)) == 0));
        if (n < (size_t)std::numeric_limits<int32_t>::max())
            _ocompact_by_half_rand_exp<DataIt, FlagIt,
                                       uint32_t>(
                data_first, flag_first, (uint32_t)n, Z, 0, std::log2(Z));
        else
            _ocompact_by_half_rand_exp<DataIt, FlagIt,
                                       size_t>(
                data_first, flag_first, n, Z, 0, std::log2(Z));
    }

    template <typename DataIt1,
              typename DataIt2,
              typename FlagIt1,
              typename FlagIt2,
              typename NumTp>
    void _ocompact_two_compacted_arrays_serial(DataIt1 data_first_1, FlagIt1 flag_first_1,
                                               DataIt2 data_first_2, FlagIt2 flag_first_2,
                                               NumTp n)
    {
        if (n == 1)
        {
            obliSwap(*data_first_1, *data_first_2, (!*flag_first_1) & (*flag_first_2));
            obliSwap(*flag_first_1, *flag_first_2, (!*flag_first_1) & (*flag_first_2));
            return;
        }
        for (NumTp i = 0; i < n; i++)
        {
            obliSwap(data_first_1[i], data_first_2[i],
                           (!flag_first_1[i]) & flag_first_2[i]);
            obliSwap(flag_first_1[i], flag_first_2[i],
                           (!flag_first_1[i]) & flag_first_2[i]);
        }

        if (n / 2 < std::numeric_limits<int32_t>::max())
        {
            _ocompact_two_compacted_arrays_serial(
                data_first_1, flag_first_1,
                data_first_1 + n / 2, flag_first_1 + n / 2,
                (uint32_t)(n / 2));
            _ocompact_two_compacted_arrays_serial(
                data_first_2, flag_first_2,
                data_first_2 + n / 2, flag_first_2 + n / 2,
                (uint32_t)(n / 2));
        }
        else
        {
            _ocompact_two_compacted_arrays_serial(
                data_first_1, flag_first_1,
                data_first_1 + n / 2, flag_first_1 + n / 2,
                n / 2);
            _ocompact_two_compacted_arrays_serial(
                data_first_2, flag_first_2,
                data_first_2 + n / 2, flag_first_2 + n / 2,
                n / 2);
        }
    }

    template <typename DataIt1,
              typename DataIt2,
              typename FlagIt1,
              typename FlagIt2,
              typename NumTp>
    void _ocompact_two_compacted_arrays(DataIt1 data_first_1, FlagIt1 flag_first_1,
                                        DataIt2 data_first_2, FlagIt2 flag_first_2,
                                        NumTp n,
                                        int threads)
    {
        if (threads <= 1)
        {
            _ocompact_two_compacted_arrays_serial(data_first_1, flag_first_1,
                                                  data_first_2, flag_first_2,
                                                  n);
            return;
        }
        if (n == 1)
        {
            obliSwap(*data_first_1, *data_first_2, (!*flag_first_1) & (*flag_first_2));
            obliSwap(*flag_first_1, *flag_first_2, (!*flag_first_1) & (*flag_first_2));
            return;
        }
        for (NumTp i = 0; i < n; i++)
        {
            obliSwap(data_first_1[i], data_first_2[i],
                           (!flag_first_1[i]) & flag_first_2[i]);
            obliSwap(flag_first_1[i], flag_first_2[i],
                           (!flag_first_1[i]) & flag_first_2[i]);
        }

        if (n / 2 < std::numeric_limits<int32_t>::max())
        {
            _ocompact_two_compacted_arrays_serial(
                data_first_1, flag_first_1,
                data_first_1 + n / 2, flag_first_1 + n / 2,
                (uint32_t)(n / 2));
            _ocompact_two_compacted_arrays_serial(
                data_first_2, flag_first_2,
                data_first_2 + n / 2, flag_first_2 + n / 2,
                (uint32_t)(n / 2));
        }
        else
        {
            _ocompact_two_compacted_arrays_serial(
                data_first_1, flag_first_1,
                data_first_1 + n / 2, flag_first_1 + n / 2,
                n / 2);
            _ocompact_two_compacted_arrays_serial(
                data_first_2, flag_first_2,
                data_first_2 + n / 2, flag_first_2 + n / 2,
                n / 2);
        }
    }

    template <typename DataIt1,
              typename DataIt2,
              typename FlagIt1,
              typename FlagIt2,
              typename NumTp>
    void _ocompact_two_compacted_arrays_serial(DataIt1 data_first_1, FlagIt1 flag_first_1,
                                               DataIt2 data_first_2, FlagIt2 flag_first_2,
                                               NumTp n,
                                               NumTp bit_budget)
    {
        assert((((n) & ((n) - 1)) == 0));
        if (bit_budget == 0)
        {
            _ocompact_two_compacted_arrays_serial(data_first_1, flag_first_1,
                                                  data_first_2, flag_first_2,
                                                  n);
            return;
        }
        if (n == 1)
        {
            if ((!*flag_first_1) & (*flag_first_2))
            {
                obliSwap(*data_first_1, *data_first_2, (!*flag_first_1) & (*flag_first_2));
                obliSwap(*flag_first_1, *flag_first_2, (!*flag_first_1) & (*flag_first_2));
            }
            return;
        }
        NumTp cnt = 0;
        for (NumTp i = 0; i < n; i++)
        {
            obliSwap(data_first_1[i], data_first_2[i],
                           (!flag_first_1[i]) & flag_first_2[i]);
            obliSwap(flag_first_1[i], flag_first_2[i],
                           (!flag_first_1[i]) & flag_first_2[i]);
            cnt += (NumTp)flag_first_2[i];
        }

        if (cnt == 0)
        {
            if (n / 2 <
                std::numeric_limits<uint32_t>::max())
                _ocompact_two_compacted_arrays_serial(
                    data_first_1, flag_first_1,
                    data_first_1 + n / 2, flag_first_1 + n / 2,
                    (uint32_t)(n / 2), (uint32_t)(bit_budget - 1));
            else
                _ocompact_two_compacted_arrays_serial(
                    data_first_1, flag_first_1,
                    data_first_1 + n / 2, flag_first_1 + n / 2,
                    n / 2, bit_budget - 1);
        }
        else
        {
            if (n / 2 <
                std::numeric_limits<uint32_t>::max())
                _ocompact_two_compacted_arrays_serial(
                    data_first_2, flag_first_2,
                    data_first_2 + n / 2, flag_first_2 + n / 2,
                    (uint32_t)(n / 2), (uint32_t)(bit_budget - 1));
            else
                _ocompact_two_compacted_arrays_serial(
                    data_first_2, flag_first_2,
                    data_first_2 + n / 2, flag_first_2 + n / 2,
                    n / 2, bit_budget - 1);
        }
    }

    /**
     * @brief Obliviously compact two compacted arrays with the same length `n` in the form of
     * @param data_first_1: `1 1 1 1 ... 0 0 0`
     * @param data_first_2: `1 1 1 ... 0 0 0 0`
     * @result the first n items of `data_first_1` || `data_first_2` are compacted, i.e.,
     * `1 1 1 ... 1 1 1 || 1 1 0 ... 0 0 0`
     */
    template <typename DataIt,
              typename FlagIt,
              typename NumTp>
    void ocompact_two_compacted_arrays(DataIt data_first_1, FlagIt flag_first_1,
                                       DataIt data_first_2, FlagIt flag_first_2,
                                       NumTp n,
                                       NumTp bit_budget,
                                       int threads = 1)
    {
        NumTp cnt = 0;
        for (NumTp i = 0; i < n; i++)
        {
            obliSwap(data_first_1[n - 1 - i], data_first_2[i],
                           (!flag_first_1[n - 1 - i]) & flag_first_2[i]);
            obliSwap(flag_first_1[n - 1 - i], flag_first_2[i],
                           (!flag_first_1[n - 1 - i]) & flag_first_2[i]);
            cnt += (NumTp)flag_first_2[i];
        }
        if (bit_budget == 0)
        {
            if (n < std::numeric_limits<uint32_t>::max())
            {
                _ocompact_two_compacted_arrays(data_first_1,
                                               flag_first_1,
                                               data_first_2, flag_first_2,
                                               (uint32_t)n, (uint32_t)bit_budget);
            }
            else
            {
                _ocompact_two_compacted_arrays(data_first_1,
                                               flag_first_1,
                                               data_first_2, flag_first_2,
                                               n, bit_budget);
            }
        }
        if (n < std::numeric_limits<uint32_t>::max())
            _ocompact_two_compacted_arrays_serial(data_first_1,
                                                  flag_first_1,
                                                  data_first_2, flag_first_2,
                                                  (uint32_t)n, (uint32_t)bit_budget);
        else
            _ocompact_two_compacted_arrays_serial(data_first_1,
                                                  flag_first_1,
                                                  data_first_2, flag_first_2,
                                                  n, bit_budget);
    }

    template <typename DataIt,
              typename FlagIt,
              typename NumTp>
    void _relaxed_ocompact(DataIt data_first, FlagIt flag_first,
                           NumTp n,
                           NumTp Z,
                           int threads = 1)
    {
        if (n <= Z)
        {
            _or_off_compact_entry(data_first, flag_first, n, 1);
            return;
        }
        for (NumTp bid = 0; bid < n / Z; bid++)
        {
            NumTp st = bid * Z;
            _or_off_compact_entry(data_first + st, flag_first + st, Z, 1);
        }
        std::partition(data_first, data_first + n, [&flag_first, &data_first](const auto &item)
                       { return *(flag_first + (&item - &*data_first)); });
        std::partition(flag_first, flag_first + n, [&flag_first](const auto &item)
                       { return *(flag_first + (&item - &*flag_first)); });
    }

    template <typename DataIt,
              typename FlagIt,
              typename NumTp>
    void relaxed_ocompact(DataIt data_first, FlagIt flag_first,
                          NumTp n,
                          NumTp Z = OCOMPACT_Z,
                          int threads = 1)
    {
        assert((((n) & ((n) - 1)) == 0));
        assert((((Z) & ((Z) - 1)) == 0));
        if (n < std::numeric_limits<uint32_t>::max())
        {
            _relaxed_ocompact(data_first, flag_first,
                              (uint32_t)n, (uint32_t)Z, threads);
        }
        else
        {
            _relaxed_ocompact(data_first, flag_first,
                              n, Z, threads);
        }
    }

} // namespace h2o2ram_ns
