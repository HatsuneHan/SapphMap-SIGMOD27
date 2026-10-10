#pragma once
#include <stdint.h>
#include <unistd.h>
#include <array>
#include <type_traits>
#include <cstring>

#ifndef CACHE_OBLIVIOUS
#define CACHE_OBLIVIOUS 0
#endif

#ifndef PAGE_LEVEL_OBLIVIOUS
#define PAGE_LEVEL_OBLIVIOUS 0
#endif

#define MIN_CAPACITY 65536

#define osorter stateless_osorter

#define OCOMPACT_Z 4096
#define EPSILON_INV 8
#define MAJOR_BIN_SIZE (EPSILON_INV * EPSILON_INV * 1024)
#define OVERFLOW_PILE_BIN_SIZE 512
// #define LINEAR_SCAN_THRESHOLD 256 // large block size
#define LINEAR_SCAN_THRESHOLD 128 // small block size
#define SMALL_HASH_TABLE_THRESHOLD LINEAR_SCAN_THRESHOLD
// Target per-construction failure probability of at most 2^-128.  With the
// current planner this selects four cuckoo choices for every cuckoo table
// (all such tables have at most MAJOR_BIN_SIZE entries).
#define DELTA_INV_LOG2 128

#define PARTIAL_SUM_PARALLEL_THRESHOLD (1 << 21)
    using Byte = uint8_t;
    constexpr size_t PAGE_SIZE = 4096;

    template <typename KeyType, std::size_t BlockSize = sizeof(KeyType)>
    struct Block
    {
        static_assert(std::is_integral_v<KeyType> && std::is_unsigned_v<KeyType>,
                      "H2O2RAM block keys must be unsigned integers");
        static_assert(BlockSize >= sizeof(KeyType),
                      "H2O2RAM block cannot be smaller than its key");
        KeyType id;
        Byte value[BlockSize - sizeof(KeyType)]{};
        Block(KeyType id = -1) : id(id) {}
        bool operator==(const Block<KeyType, BlockSize> &other) const
        {
            bool ret = (id == other.id);
            for (size_t i = 0; i < BlockSize - sizeof(KeyType); i++)
                ret &= (value[i] == other.value[i]);
            return ret;
        }

        // Constructor from any arithmetic type that fits into the value array
        template <typename T>
        Block(T input, KeyType id = -1) : id(id)
        {
            static_assert(std::is_trivially_copyable_v<T>,
                          "H2O2RAM block values must be trivially copyable");
            static_assert(sizeof(T) == BlockSize - sizeof(KeyType),
                          "H2O2RAM block value has the wrong size");
            std::memcpy(value, &input, sizeof(T)); // Copy new data
        }

        bool dummy() const
        {
            // check if the msb is 1
            return id & (KeyType(1) << (sizeof(KeyType) * 8 - 1));
        }

        // Template function to get the value as a basic type
        template <typename T>
        explicit operator T() const
        {
            static_assert(std::is_trivially_copyable_v<T>,
                          "H2O2RAM block conversion requires a trivial type");
            static_assert(sizeof(T) == BlockSize,
                          "H2O2RAM block conversion has the wrong size");
            T output;
            std::memcpy(&output, this, sizeof(T));
            return output;
        }
    };

    // POD pair for ExtVector compatibility (replaces std::pair<KeyType, Block_t>)
    template <typename KeyType, std::size_t BlockSize = sizeof(KeyType)>
    struct PairBlock
    {
        KeyType first;
        Block<KeyType, BlockSize> second;
        PairBlock() : first(0), second() {}
        PairBlock(KeyType f, Block<KeyType, BlockSize> s) : first(f), second(s) {}
    };
