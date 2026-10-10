#pragma once
#include "common/encrypted.hpp"

// EnigMap names this wrapper type; map it to the canonical encrypted type.
template <typename T>
using MixedEncrypted = Encrypted<T>;