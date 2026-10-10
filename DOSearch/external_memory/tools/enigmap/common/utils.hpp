#pragma once
#include "common/utils.hpp"

INLINE uint64_t GetLogBaseTwo(uint64_t n) {
	Assert(n > 0);
	return FloorLog2(n);
}