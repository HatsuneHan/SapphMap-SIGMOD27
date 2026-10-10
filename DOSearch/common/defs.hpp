#pragma once
#include "common/cpp_extended.hpp"

#ifndef SERVER__CACHE_SIZE
#define SERVER__CACHE_SIZE 8192
#endif

#define ORAM_USE_INRAM_SERVER true

#ifndef ENCLAVE_SIZE
#define ENCLAVE_SIZE 65536000
#endif
#ifndef DEFAULT_HEAP_SIZE
#define DEFAULT_HEAP_SIZE ((((uint64_t)(ENCLAVE_SIZE)) / (1024ULL * 1024ULL)) * 0xEA000ULL)
#endif

#ifndef BLOCK_SIZE
#define BLOCK_SIZE 32768
#endif

#ifndef BUFFER_SIZE
#define BUFFER_SIZE (512ULL * 1024)
#endif

#ifndef GAMMA
#define GAMMA 2
#endif

#ifndef EPS_TOTAL
#define EPS_TOTAL 4.0
#endif

#define FLAMEGRAPHS_BASE_FOLDER "./quality/flamegraphs/"