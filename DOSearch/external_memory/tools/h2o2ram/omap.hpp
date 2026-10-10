#pragma once
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <random>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#include "oram.hpp"
#include "hash_planner.hpp"

namespace h2o2ram_ns {

    template <typename KeyType, typename ValueType>
    class ObliviousMap
    {
    private:
        ObliviousRAM<size_t, ValueType> oram;

    public:
        inline void insert(const KeyType &key, const ValueType &value)
        {
            size_t hash = std::hash<KeyType>{}(key);
            hash &= ~(1UL << (sizeof(size_t) * 8 - 1));
            oram.insert(hash, value);
        }

        inline void erase(const KeyType &key)
        {
            size_t hash = std::hash<KeyType>{}(key);
            hash &= ~(1UL << (sizeof(size_t) * 8 - 1));
            oram.erase(hash);
        }

        inline ValueType &operator[](const KeyType &key)
        {
            size_t hash = std::hash<KeyType>{}(key);
            hash &= ~(1UL << (sizeof(size_t) * 8 - 1));
            return oram[hash];
        }

        inline void flush() { oram.flush(); }
    };

    template <typename ValueType>
    class ObliviousMap<uint16_t, ValueType>
    {
    private:
        ObliviousRAM<uint16_t, ValueType> oram;

    public:
        inline void insert(const uint16_t key, const ValueType &value)
        {
            oram.insert(key, value);
        }

        inline void erase(const uint16_t key)
        {
            oram.erase(key);
        }

        inline ValueType &operator[](const uint16_t key)
        {
            return oram[key];
        }

        inline void flush() { oram.flush(); }
    };

    template <typename ValueType>
    class ObliviousMap<uint32_t, ValueType>
    {
    private:
        ObliviousRAM<uint32_t, ValueType> oram;

    public:
        inline void insert(const uint32_t key, const ValueType &value)
        {
            oram.insert(key, value);
        }

        inline void erase(const uint32_t key)
        {
            oram.erase(key);
        }

        inline ValueType &operator[](const uint32_t key)
        {
            return oram[key];
        }

        inline void flush() { oram.flush(); }
    };

    template <typename ValueType>
    class ObliviousMap<uint64_t, ValueType>
    {
    private:
        using RAM = ObliviousRAM<uint64_t, ValueType>;
        RAM oram;

    public:
        using Stats = typename RAM::Stats;
        using Diagnostics = typename RAM::Diagnostics;

        ObliviousMap() = default;

        explicit ObliviousMap(uint64_t capacity)
            : oram(capacity) {}

        inline void insert(const uint64_t key, const ValueType &value)
        {
            oram.insert(key, value);
        }

        inline void erase(const uint64_t key)
        {
            oram.erase(key);
        }

        inline ValueType &operator[](const uint64_t key)
        {
            return oram[key];
        }

        inline void flush() { oram.flush(); }

        inline uint64_t size() const { return oram.size(); }
        inline Stats stats() const { return oram.stats(); }
        inline void reset_stats() const { oram.reset_stats(); }
        inline Diagnostics diagnostics() const { return oram.diagnostics(); }
        inline size_t level_count() const { return oram.level_count(); }
        inline uint64_t level_capacity(size_t level) const {
            return oram.level_capacity(level);
        }
        inline bool level_occupied(size_t level) const {
            return oram.level_occupied(level);
        }

        inline void prepare_synthetic_compact(
            uint64_t logical_size, const uint64_t* query_keys,
            size_t query_count, uint64_t deterministic_seed) {
            oram.prepare_synthetic_compact(logical_size, query_keys,
                                           query_count,
                                           deterministic_seed);
        }

        inline void reset_synthetic_compact() {
            oram.reset_synthetic_compact();
        }
    };

} // namespace h2o2ram_ns
