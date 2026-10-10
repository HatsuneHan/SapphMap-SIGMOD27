#pragma once
#include "h2o2ram_aes.hpp"
#include <string.h>
#include <type_traits>

namespace h2o2ram_ns {

template <typename KeyType>
class AESPRF
{
private:
    H2O2RAM::AESKey aes_key;
    KeyType range;

public:
    AESPRF(KeyType range = 0) : range(range)
    {
        reset();
    }

    AESPRF &operator=(const AESPRF &other)
    {
        if (this != &other)
        {
            range = other.range;
            aes_key = other.aes_key;
        }
        return *this;
    }

    void reset()
    {
        uint8_t aes_key_bytes[H2O2RAM::kAESBlockSize];
        H2O2RAM::hash_random_bytes(aes_key_bytes,
                                   H2O2RAM::kAESBlockSize);
        aes_key.set_key(aes_key_bytes);
    }

    KeyType operator()(KeyType input) const
    {
        uint8_t in[H2O2RAM::kAESBlockSize] = {};
        uint8_t out[H2O2RAM::kAESBlockSize] = {};

        std::memcpy(in, &input, sizeof(input));
        aes_key.encrypt(in, out);

        KeyType output;
        std::memcpy(&output, out, sizeof(output));

        return output % range;
    }
};

} // namespace h2o2ram_ns
