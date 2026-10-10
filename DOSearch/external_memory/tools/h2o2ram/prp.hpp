#pragma once
#include "h2o2ram_aes.hpp"
#include <stdint.h>
#include <array>

namespace h2o2ram_ns {

    class AESCSPRP
    {
    private:
        H2O2RAM::AESKey enc_key;

    public:
        AESCSPRP()
        {
            uint8_t key_bytes[H2O2RAM::kAESBlockSize];
            H2O2RAM::AESRNG rng;
            rng.generate(key_bytes, H2O2RAM::kAESBlockSize);
            enc_key.set_key(key_bytes);
        }

        std::array<uint8_t, H2O2RAM::kAESBlockSize> operator()(const uint8_t *input) const
        {
            std::array<uint8_t, H2O2RAM::kAESBlockSize> output;
            enc_key.encrypt(input, output.data());
            return output;
        }
    };

} // namespace h2o2ram_ns
