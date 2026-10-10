#pragma once

/**
 * @file h2o2ram_aes.hpp
 * @brief AES block encryption and enclave-seeded RNG wrappers for H2O2RAM.
 *
 * BearSSL exposes AES-CTR in this build.  Encrypting an all-zero plaintext
 * with the input block split into CTR's 12-byte nonce and 4-byte counter is
 * exactly AES(key, input), which matches OpenSSL AES_encrypt used upstream.
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <array>
#include <random>

#include "bearssl_block.h"
#include "bearssl_rand.h"
#include "bearssl_hash.h"

#ifdef ENCLAVE_MODE
#include "sgx_trts.h"
#endif

namespace H2O2RAM {

inline constexpr size_t kAESBlockSize = 16;

// Deterministic seeds are limited to internal topology construction helpers.
// Page-encryption nonces continue to use the normal SGX RNG.
inline bool g_deterministic_rng = false;
inline uint64_t g_deterministic_seed = 0;
inline uint64_t g_deterministic_counter = 0;

inline uint64_t splitmix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

inline void set_deterministic_rng(uint64_t seed) {
    g_deterministic_rng = true;
    g_deterministic_seed = seed;
    g_deterministic_counter = 0;
}

inline void clear_deterministic_rng() {
    g_deterministic_rng = false;
    g_deterministic_counter = 0;
}

/// AES key context using BearSSL x86-NI CTR mode.
struct AESKey {
    br_aes_x86ni_ctr_keys ctx;

    void set_key(const uint8_t* key16) {
        br_aes_x86ni_ctr_init(&ctx, key16, 16);
    }

    /// Encrypt one AES block (not CTR-encrypt the input as plaintext).
    void encrypt(const uint8_t* in16, uint8_t* out16) const {
        uint8_t iv[12];
        std::memcpy(iv, in16, sizeof(iv));
        const uint32_t counter =
            (static_cast<uint32_t>(in16[12]) << 24) |
            (static_cast<uint32_t>(in16[13]) << 16) |
            (static_cast<uint32_t>(in16[14]) << 8) |
            static_cast<uint32_t>(in16[15]);
        std::memset(out16, 0, kAESBlockSize);
        br_aes_x86ni_ctr_run(&ctx, iv, counter, out16, kAESBlockSize);
    }
};

/// Cryptographic RNG using BearSSL's HMAC-DRBG with SHA-256.
struct AESRNG {
    br_hmac_drbg_context ctx;

    AESRNG() {
        std::array<uint8_t, 32> seed{};
#ifdef ENCLAVE_MODE
        if (sgx_read_rand(seed.data(), seed.size()) != SGX_SUCCESS)
            std::abort();
#else
        std::random_device rd;
        for (auto &byte : seed)
            byte = static_cast<uint8_t>(rd());
#endif
        br_hmac_drbg_init(&ctx, &br_sha256_vtable,
                          seed.data(), seed.size());
    }

    void generate(uint8_t* out, size_t len) {
        br_hmac_drbg_generate(&ctx, out, len);
    }
};

inline uint32_t secure_seed() {
    if (g_deterministic_rng) {
        return static_cast<uint32_t>(
            splitmix64(g_deterministic_seed + g_deterministic_counter++));
    }
    AESRNG rng;
    uint32_t seed = 0;
    rng.generate(reinterpret_cast<uint8_t*>(&seed), sizeof(seed));
    return seed;
}

inline void hash_random_bytes(uint8_t* out, size_t len) {
    if (!g_deterministic_rng) {
        AESRNG rng;
        rng.generate(out, len);
        return;
    }
    size_t offset = 0;
    while (offset < len) {
        const uint64_t word =
            splitmix64(g_deterministic_seed + g_deterministic_counter++);
        const size_t take = std::min(sizeof(word), len - offset);
        std::memcpy(out + offset, &word, take);
        offset += take;
    }
}

} // namespace H2O2RAM
