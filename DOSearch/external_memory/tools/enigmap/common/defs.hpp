#pragma once
#include "external_memory/tools/enigmap/common/cpp_extended.hpp"

// ORAM bucket sizes:
// For PathORAM bucket size = ORAM__Z, for RingORAM = ORAM__Z + ORAM__S
#ifndef ORAM__Z
#define ORAM__Z 4
#endif
#ifndef ORAM__S
#define ORAM__S 2
#endif

// Parameter A from Ring ORAM (number of accesses between reshufles)
#ifndef ORAM__A
#define ORAM__A 3
#endif

// ORAM doubly oblivious stash size:
#ifndef ORAM__MS
#define ORAM__MS 16384
#endif

// At what levels should encryption be used:
#ifndef ORAM_SERVER__ENCRYPT_LARGE_BUCKETS
#define ORAM_SERVER__ENCRYPT_LARGE_BUCKETS false
#endif
#ifndef ORAM__ENCRYPT_BUCKETS
#define ORAM__ENCRYPT_BUCKETS false
#endif
#ifndef ORAM__ENCRYPT_BLOCKS
#define ORAM__ENCRYPT_BLOCKS false
#endif

// Info regarding emdas boas packing:
#ifndef ORAM_SERVER__DIRECTLY_CACHED_LEVELS
#define ORAM_SERVER__DIRECTLY_CACHED_LEVELS 16
#endif
#ifndef ORAM_SERVER__BUCKET_CACHE_SIZE
#define ORAM_SERVER__BUCKET_CACHE_SIZE 8192
#endif
#ifndef ORAM_SERVER__CACHE_SIZE
#define ORAM_SERVER__CACHE_SIZE 8192
#endif
#ifndef ORAM_SERVER__LEVELS_PER_PACK
#define ORAM_SERVER__LEVELS_PER_PACK 4
#endif

#ifndef ORAM_USE_INRAM_SERVER
#define ORAM_USE_INRAM_SERVER true
#endif

#ifndef ENCLAVE_SIZE
#define ENCLAVE_SIZE 128
#endif
#ifndef DEFAULT_HEAP_SIZE
#define DEFAULT_HEAP_SIZE ((((uint64_t)(ENCLAVE_SIZE)) / (1024ULL * 1024ULL)) * 0xEA000ULL)
#endif

// ORAM block size:
// #define BLOCK_SIZE 4096
// ORAM plaintext size (4096 - 16 * 2)
// #define PT_SIZE 4064
// ORAM data size (4096 - 16 * 2 - 8 * 2)
// #define DATA_SIZE 4048
// Multiplier for stash limit
#ifndef STASH_MULTIPLIER
#define STASH_MULTIPLIER 8
#endif

#define FLAMEGRAPHS_BASE_FOLDER "./quality/flamegraphs/"
// #define FLAMEGRAPHS_BASE_FOLDER "./"