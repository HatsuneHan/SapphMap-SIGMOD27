#pragma once

#include <inttypes.h>
#include <map>
#include <cassert>
#include <type_traits>
#include "types.hpp"
#include "oblivious_operations.hpp"

// This file implements https://eprint.iacr.org/2014/185.pdf with our optimizations.

    template <typename T>
    inline void TSET(bool cond, T &dst, const T &a, const T &b)
    {
        CMOV(cond, dst, b);
        CMOV(!cond, dst, a);
    }
    template <typename T>
    inline void CTSET(bool outer_cond, bool inner_cond, T &dst, const T &a, const T &b)
    {
        CMOV(outer_cond & inner_cond, dst, b);
        CMOV(outer_cond & !inner_cond, dst, a);
    }
