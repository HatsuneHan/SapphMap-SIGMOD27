#pragma once

#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include "common/defs.hpp"
#include "common/encutils.hpp"
#include "common/mov_intrinsics.hpp"

constexpr INLINE uint64_t GetNextPowerOfTwo(uint64_t n) {
  Assert(n <= 0x8000'0000'0000'0000);
  n = n - 1;
  n |= n >> 1;
  n |= n >> 2;
  n |= n >> 4;
  n |= n >> 8;
  n |= n >> 16;
  n |= n >> 32;
  n = n + 1;
  return n;
}

INLINE uint64_t FloorLog2(uint64_t n) {
  const uint64_t masks[6] = {0x2,    0xC,         0xF0,
                             0xFF00, 0xFFFF'0000, 0xFFFF'FFFF'0000'0000};
  uint64_t c = 32;
  uint64_t r = 0;
  for (int32_t i = 5; i >= 0; i--) {
    const bool cond = n & masks[i];
    obliMove(cond, n, n >> c);
    obliMove(cond, r, r | c);
    c >>= 1;
  }
  return r;
}

INLINE uint64_t CeilLog2(uint64_t x) {
  const uint64_t floor = FloorLog2(x);
  const bool isPow2 = ((x & (x - 1)) == 0);
  uint64_t ceil = floor + 1;
  obliMove(isPow2, ceil, floor);
  return ceil;
}

template <typename T>
INLINE T obliMin(T a, T b) {
  const bool take_b = (b < a);
  T res = a;
  obliMove(take_b, res, b);
  return res;
}

template <typename T>
INLINE T obliMax(T a, T b) {
  const bool take_b = (b > a);
  T res = a;
  obliMove(take_b, res, b);
  return res;
}

// Note: This function is oblivious but approximate.
// Rounds to nearest integer, ties away from zero.
INLINE int64_t obliRound(double x) {
  constexpr double kMaxSafe = 9223372036854774784.0;
  constexpr double kMinSafe = -9223372036854774784.0;

  const bool is_too_large = (x > kMaxSafe);
  const bool is_too_small = (x < kMinSafe);
  const bool is_nan = (x != x);

  double x_safe = x;
  obliMove(is_too_large, x_safe, kMaxSafe);
  obliMove(is_too_small, x_safe, kMinSafe);
  obliMove(is_nan, x_safe, 0.0);

  uint64_t ux = 0;
  std::memcpy(&ux, &x_safe, sizeof(ux));
  const uint64_t sign_bit = ux & 0x8000000000000000ULL;

  const double half = 0.5;
  uint64_t uhalf = 0;
  std::memcpy(&uhalf, &half, sizeof(uhalf));
  uhalf |= sign_bit;

  double signed_half = 0.0;
  std::memcpy(&signed_half, &uhalf, sizeof(signed_half));

  return static_cast<int64_t>(x_safe + signed_half);
}

INLINE int64_t obliFloor(double x) {
  constexpr double kMaxSafe = 9223372036854774784.0;
  constexpr double kMinSafe = -9223372036854774784.0;

  const bool is_too_large = (x > kMaxSafe);
  const bool is_too_small = (x < kMinSafe);
  const bool is_nan = (x != x);

  double x_safe = x;
  obliMove(is_too_large, x_safe, kMaxSafe);
  obliMove(is_too_small, x_safe, kMinSafe);
  obliMove(is_nan, x_safe, 0.0);

  int64_t res = static_cast<int64_t>(x_safe);
  const bool needs_decrement = (x_safe < static_cast<double>(res));
  obliMove(needs_decrement, res, res - 1);

  return res;
}

INLINE int64_t obliCeil(double x) {
  constexpr double kMaxSafe = 9223372036854774784.0;
  constexpr double kMinSafe = -9223372036854774784.0;

  const bool is_too_large = (x > kMaxSafe);
  const bool is_too_small = (x < kMinSafe);
  const bool is_nan = (x != x);

  double x_safe = x;
  obliMove(is_too_large, x_safe, kMaxSafe);
  obliMove(is_too_small, x_safe, kMinSafe);
  obliMove(is_nan, x_safe, 0.0);

  int64_t res = static_cast<int64_t>(x_safe);
  const bool needs_increment = (x_safe > static_cast<double>(res));
  obliMove(needs_increment, res, res + 1);

  return res;
}

INLINE uint64_t obliFloorU64(double x) {
  assert(x >= 0.0);

  constexpr double kMaxSafeUInt = 18446744073709549568.0;
  const bool is_too_large = (x >= kMaxSafeUInt);
  const bool is_nan = (x != x);

  double x_safe = x;
  obliMove(is_too_large, x_safe, kMaxSafeUInt);
  obliMove(is_nan, x_safe, 0.0);

  uint64_t res = static_cast<uint64_t>(x_safe);
  obliMove(is_too_large, res, std::numeric_limits<uint64_t>::max());

  return res;
}

INLINE uint64_t obliCeilU64(double x) {
  assert(x >= 0.0);

  constexpr double kMaxSafeUInt = 18446744073709549568.0;
  const bool is_too_large = (x >= kMaxSafeUInt);
  const bool is_nan = (x != x);

  double x_safe = x;
  obliMove(is_too_large, x_safe, kMaxSafeUInt);
  obliMove(is_nan, x_safe, 0.0);

  uint64_t res = static_cast<uint64_t>(x_safe);
  const bool needs_increment = (x_safe > static_cast<double>(res));
  obliMove(needs_increment, res, res + 1);

  obliMove(is_too_large, res, std::numeric_limits<uint64_t>::max());
  return res;
}

// Note: This function is oblivious but approximate.
INLINE double obliExp(double x) {
  constexpr double kInvLn2 = 1.4426950408889634;
  constexpr double kMaxInput = 709.0;
  constexpr double kMinInput = -708.3964185322641;

  const bool is_too_large = (x > kMaxInput);
  const bool is_too_small = (x < kMinInput);

  double x_safe = x;
  obliMove(is_too_large, x_safe, kMaxInput);
  obliMove(is_too_small, x_safe, kMinInput);

  constexpr double kMagic = 6755399441055744.0;  // 3 * 2^51
  const double k_real = x_safe * kInvLn2;
  double k_round = k_real + kMagic;
  k_round -= kMagic;
  const int64_t k = static_cast<int64_t>(k_round);

  constexpr double kLn2Hi = 6.93147180369123816490e-01;
  constexpr double kLn2Lo = 1.90821492927058770002e-10;
  double r = x_safe - k_round * kLn2Hi;
  r -= k_round * kLn2Lo;

  double p = 1.98412698298579493134e-04;
  p = p * r + 1.38888888889829428080e-03;
  p = p * r + 8.33333333332248946124e-03;
  p = p * r + 4.16666666666663879199e-02;
  p = p * r + 1.66666666666666019037e-01;
  p = p * r + 5.00000000000000000000e-01;
  p = p * r + 1.00000000000000000000e+00;
  const double poly = p * r + 1.0;

  const uint64_t scale_bits = (static_cast<uint64_t>(k + 1023)) << 52;
  double scale = 0.0;
  std::memcpy(&scale, &scale_bits, sizeof(scale));

  double result = poly * scale;

  const double val_inf = std::numeric_limits<double>::infinity();
  const double val_zero = 0.0;
  obliMove(is_too_large, result, val_inf);
  obliMove(is_too_small, result, val_zero);

  return result;
}

// Note: This function is oblivious but approximate.
INLINE double obliLog(double x) {
  const bool is_le_zero = (x <= 0.0);
  const bool is_inf = (x == std::numeric_limits<double>::infinity());
  const bool is_nan = (x != x);

  double x_safe = x;
  obliMove(is_le_zero, x_safe, 1.0);
  obliMove(is_nan, x_safe, 1.0);

  uint64_t ux = 0;
  std::memcpy(&ux, &x_safe, sizeof(ux));
  const int64_t raw_exp = (ux >> 52) & 0x7FF;
  const bool is_subnormal = (raw_exp == 0);

  constexpr double kTwoPow54 = 18014398509481984.0;  // 2^54
  const double x_scaled = x_safe * kTwoPow54;
  double x_norm = x_safe;
  obliMove(is_subnormal, x_norm, x_scaled);

  uint64_t ux_norm = 0;
  std::memcpy(&ux_norm, &x_norm, sizeof(ux_norm));
  const int64_t raw_exp_norm = (ux_norm >> 52) & 0x7FF;
  int64_t exp = raw_exp_norm - 1023;
  const int64_t exp_sub = exp - 54;
  obliMove(is_subnormal, exp, exp_sub);

  const uint64_t mantissa =
      (ux_norm & 0x000FFFFFFFFFFFFFULL) | 0x3FF0000000000000ULL;
  double m = 0.0;
  std::memcpy(&m, &mantissa, sizeof(m));

  constexpr double kSqrt2 = 1.41421356237309504880;
  const bool large_m = (m > kSqrt2);
  const double m_scaled = m * 0.5;
  obliMove(large_m, m, m_scaled);
  const int64_t exp_plus_1 = exp + 1;
  obliMove(large_m, exp, exp_plus_1);

  const double z = m - 1.0;
  double p = -0.055555555555555556;
  p = p * z + 0.058823529411764706;
  p = p * z - 0.062500000000000000;
  p = p * z + 0.066666666666666667;
  p = p * z - 0.071428571428571425;
  p = p * z + 0.076923076923076923;
  p = p * z - 0.083333333333333333;
  p = p * z + 0.090909090909090912;
  p = p * z - 0.100000000000000000;
  p = p * z + 0.111111111111111111;
  p = p * z - 0.125000000000000000;
  p = p * z + 0.142857142857142857;
  p = p * z - 0.166666666666666667;
  p = p * z + 0.200000000000000000;
  p = p * z - 0.250000000000000000;
  p = p * z + 0.333333333333333333;
  p = p * z - 0.500000000000000000;
  p = p * z + 1.000000000000000000;
  const double log_m = z * p;

  constexpr double kLn2Hi = 6.93147180369123816490e-01;
  constexpr double kLn2Lo = 1.90821492927058770002e-10;
  const double exp_dbl = static_cast<double>(exp);
  double result = log_m + exp_dbl * kLn2Lo + exp_dbl * kLn2Hi;

  const double val_neg_inf = -std::numeric_limits<double>::infinity();
  const double val_inf = std::numeric_limits<double>::infinity();
  const double val_nan = std::numeric_limits<double>::quiet_NaN();
  const bool is_zero = (x == 0.0);
  const bool is_neg = (x < 0.0);

  obliMove(is_zero, result, val_neg_inf);
  obliMove(is_neg, result, val_nan);
  obliMove(is_inf, result, val_inf);
  obliMove(is_nan, result, val_nan);

  return result;
}

extern RandGen default_rand;

// [left,right]
inline uint64_t UniformRandom(uint64_t left, uint64_t right) {
  return default_rand.rand64() % (right - left + 1) + left;
}

inline uint32_t UniformRandom32(uint32_t left, uint32_t right) {
  return default_rand.rand32() % (right - left + 1) + left;
}

inline bool UniformRandomBit() { return default_rand.rand1(); }

// [0,right]
INLINE uint64_t UniformRandom(uint64_t right) {
  return UniformRandom(0, right);
}

// [0,right]
INLINE uint64_t UniformRandom() { return default_rand.rand64(); }

INLINE uint64_t UniformRandom32(uint32_t right) {
  return UniformRandom32(0, right);
}

// [0,right]
INLINE uint64_t UniformRandom32() { return default_rand.rand32(); }

INLINE double UniformRandomDoubleExclusive() {
  constexpr double kInvDenom = 
      1.0 / (static_cast<double>(std::numeric_limits<uint64_t>::max()) + 2.0);
  return (static_cast<double>(UniformRandom()) + 1.0) * kInvDenom;
}

INLINE double UniformRandomDoubleInclusive() {
  constexpr double kInvDenom = 
      1.0 / static_cast<double>(std::numeric_limits<uint64_t>::max());
  return static_cast<double>(UniformRandom()) * kInvDenom;
}

INLINE void GetRand16(uint8_t* out) {
  *(uint64_t*)out = UniformRandom();
  *(uint64_t*)(out + 8) = UniformRandom();
}

// x/y round up
INLINE uint64_t divRoundUp(size_t x, size_t y) { return (x + y - 1) / y; }

template <typename Iterator>
static auto getVecProduct(Iterator begin, Iterator end) {
  using NumType = typename std::iterator_traits<Iterator>::value_type;
  NumType product = 1;
  for (auto it = begin; it != end; ++it) {
    product *= *it;
  }
  return product;
}

template <typename Vec>
static auto getVecProduct(const Vec& vec) {
  return getVecProduct(vec.begin(), vec.end());
}

/**
 * Note: this function is not oblivious
 */
template <typename Iterator>
void fisherYatesShuffle(Iterator begin, Iterator end) {
  size_t N = end - begin;
  for (size_t n = N - 1; n; --n) {
    size_t randPos = UniformRandom(n);
    std::swap(*(begin + randPos), *(--end));
  }
}
/**
 * Note: this function is not oblivious.
 * Merge two sorted vectors without deduplication. Each element is kept in
 * sorted order; duplicates across v1 and v2 are not removed. A second vector
 * records the source of each element: 0 = v1, 1 = v2, 2 = optional tail.
 * Tail is always appended when provided.
 * outSize is computed by adding one when tailValue is provided, so the
 * returned vectors have exact size.
 */
template <template <class, auto...> class VectorType>
std::pair<VectorType<uint64_t>, VectorType<uint8_t>> MergeSorted(
    VectorType<uint64_t>& v1, VectorType<uint64_t>& v2,
    const uint64_t* tailValue = nullptr, uint32_t inAuth = 0) {
  const uint64_t n1 = v1.size();
  const uint64_t n2 = v2.size();

  const uint64_t outSize = n1 + n2 + (tailValue != nullptr ? 1 : 0);

  std::pair<VectorType<uint64_t>, VectorType<uint8_t>> mergeResult{
      VectorType<uint64_t>(outSize), VectorType<uint8_t>(outSize)};
  typename VectorType<uint64_t>::Writer writerVal(mergeResult.first.begin(),
                                                  mergeResult.first.end());
  typename VectorType<uint8_t>::Writer writerSrc(mergeResult.second.begin(),
                                                mergeResult.second.end());
  typename VectorType<uint64_t>::Reader r1(v1.begin(), v1.end(), inAuth);
  typename VectorType<uint64_t>::Reader r2(v2.begin(), v2.end(), inAuth);

  uint64_t idx1 = 0, idx2 = 0;
  bool v1Valid = (n1 > 0), v2Valid = (n2 > 0);
  uint64_t curr1 = 0, curr2 = 0;
  if (v1Valid) {
    curr1 = r1.read();
    ++idx1;
  }
  if (v2Valid) {
    curr2 = r2.read();
    ++idx2;
  }

  auto proceed = [](auto& reader, uint64_t& idx, const uint64_t n,
                    uint64_t& curr, bool& valid) {
    if (idx < n) {
      curr = reader.read();
      ++idx;
    } else {
      valid = false;
    }
  };

  while (v1Valid || v2Valid) {
    const bool take1 = v1Valid && (!v2Valid || (curr1 <= curr2));
    if (take1) {
      writerVal.write(curr1);
      writerSrc.write(0);
      proceed(r1, idx1, n1, curr1, v1Valid);
    } else {
      writerVal.write(curr2);
      writerSrc.write(1);
      proceed(r2, idx2, n2, curr2, v2Valid);
    }
  }

  if (tailValue != nullptr) {
    writerVal.write(*tailValue);
    writerSrc.write(2);
  }

  writerVal.flush();
  writerSrc.flush();
  return mergeResult;
}