#pragma once

#include <cstdint>

#include "common/mov_intrinsics.hpp"

// EnigMap expects generic CMOV/CXCHG/TSET interfaces in addition to the
// low-level intrinsics helpers. Reuse the canonical DOSearch primitives.
template <typename T>
INLINE void CMOV(const uint64_t& condition, T& A, const T& B) {
  obliMove(static_cast<bool>(condition), A, B);
}

template <>
INLINE void CMOV<bool>(const uint64_t& condition, bool& A, const bool& B) {
  CMOV_BOOL(condition, A, B);
}

template <>
INLINE void CMOV<uint8_t>(const uint64_t& condition, uint8_t& A,
                          const uint8_t& B) {
  CMOV1(static_cast<bool>(condition), A, B);
}

template <>
INLINE void CMOV<uint16_t>(const uint64_t& condition, uint16_t& A,
                           const uint16_t& B) {
  CMOV2(static_cast<bool>(condition), A, B);
}

template <>
INLINE void CMOV<uint32_t>(const uint64_t& condition, uint32_t& A,
                           const uint32_t& B) {
  CMOV4(static_cast<bool>(condition), A, B);
}

template <>
INLINE void CMOV<uint64_t>(const uint64_t& condition, uint64_t& A,
                           const uint64_t& B) {
  CMOV8(static_cast<bool>(condition), A, B);
}

template <typename T>
INLINE void CXCHG(const uint64_t& condition, T& A, T& B) {
  obliSwap(static_cast<bool>(condition), A, B);
}

template <typename T>
INLINE void TSET(const uint64_t& condition, T& A, const T& B, const T& C) {
	A = B;
	CMOV(condition, A, C);
}

template <typename T>
INLINE void CTSET(const uint64_t& cond, const uint64_t& condition, T& A,
									const T& B, const T& C) {
	T tmp = A;
	TSET(condition, tmp, B, C);
	CMOV(cond, A, tmp);
}

// EnigMap source uses this macro to emit explicit overloads for some types.
#define OVERLOAD_TSET_CXCHG(T, ...)                                          \
  template <__VA_ARGS__>                                                     \
  INLINE void TSET(const uint64_t& condition, T& A, const T& B,              \
                   const T& C) {                                              \
    A = B;                                                                    \
    CMOV(condition, A, C);                                                    \
  }                                                                           \
  template <__VA_ARGS__>                                                     \
  INLINE void CXCHG(const uint64_t& condition, T& A, T& B) {                 \
    const T tmp = A;                                                          \
    CMOV(condition, A, B);                                                    \
    CMOV(condition, B, tmp);                                                  \
  }