#pragma once

/**
 * @file pds_def.hpp
 * @brief Core type definitions for SapphMap partitioned data structures.
 *
 * This header defines the canonical item layout shared by SapphMap, KVLSMTree,
 * and benchmark drivers, together with generic containers used by the
 * privacy-preserving partitioned structure (PrivDS).
 */

#include <cstring>
#include <memory>

#include "common/defs.hpp"
#include "external_memory/extemvector.hpp"
#include "external_memory/noncachedvector.hpp"
#include "external_memory/stdvector.hpp"

namespace EM::Definition {

/**
 * @brief Logical operation tag stored with each versioned item.
 */

enum class OpType : uint8_t {
  INS,  // Insert
  DEL,  // Delete
  DMY   // Dummy (padding / sentinel)
};

/// Total payload size in bytes (everything except the 8-byte key_).
/// Controlled at build time via -DPAYLOAD=N; default is 24 (original layout).
/// Minimum is 16 (= value_ 8B + version_/op_/dummy_ 8B, no padding).
#ifndef PAYLOAD
#define PAYLOAD 24
#endif

static_assert(PAYLOAD >= 16 && PAYLOAD % 8 == 0,
              "PAYLOAD must be >= 16 and a multiple of 8");
static_assert(PAYLOAD - 16 >= 0,
              "PAYLOAD - 16 must be non-negative (padding array size)");

/// Key-value item with operation type, version, and dummy flag.
/// Ordering: primary by key ascending, secondary by version descending
/// (newer versions sort first within the same key).
///
/// Layout (8 + PAYLOAD bytes, naturally aligned):
///   uint64_t key_      [8B]
///   uint64_t value_    [8B]
///   uint64_t version_  : 48  (6B, max ~281 trillion)
///   uint64_t op_       : 8   (1B)
///   uint64_t dummy_    : 8   (1B)
///   uint8_t  padding_  [PAYLOAD - 16B, zero-filled]
struct Item {
 private:
  uint64_t key_;
  uint64_t value_;
  uint64_t version_ : 48;
  uint64_t op_      : 8;
  uint64_t dummy_   : 8;
  uint8_t  padding_[PAYLOAD - 16];

 public:
  /// Maximum representable version (2^48 - 1).
  static constexpr uint64_t kVerMax = (1ULL << 48) - 1;

  Item(OpType op, uint64_t key, uint64_t value, uint64_t version)
      : key_(key), value_(value), version_(version),
        op_(static_cast<uint64_t>(op)), dummy_(0) {
    std::memset(padding_, 0, sizeof(padding_));
  }

  Item(OpType op, uint64_t key, uint64_t value, uint64_t version, bool dummy)
      : key_(key), value_(value), version_(version),
        op_(static_cast<uint64_t>(op)), dummy_(dummy) {
    std::memset(padding_, 0, sizeof(padding_));
  }

  /// Default-constructs a dummy sentinel (key = value = UINT64_MAX, version = 0).
  Item() : key_(UINT64_MAX), value_(UINT64_MAX), version_(0),
           op_(static_cast<uint64_t>(OpType::DMY)), dummy_(1) {
    std::memset(padding_, 0, sizeof(padding_));
  }

  INLINE uint64_t get_key() const { return key_; }
  INLINE uint64_t get_value() const { return value_; }
  INLINE uint64_t get_ver() const { return version_; }
  INLINE OpType get_op() const { return static_cast<OpType>(op_); }
  INLINE bool is_dummy() const { return static_cast<bool>(dummy_); }

  /// Marks this item as a dummy sentinel, overwriting all fields.
  INLINE void set_dummy() {
    key_ = UINT64_MAX;
    value_ = UINT64_MAX;
    version_ = kVerMax;
    op_ = static_cast<uint64_t>(OpType::DMY);
    dummy_ = 1;
    std::memset(padding_, 0, sizeof(padding_));
  }

  INLINE bool operator==(const Item& other) const {
    return key_ == other.key_ && version_ == other.version_;
  }
  INLINE bool operator!=(const Item& other) const { return !(*this == other); }
  INLINE bool operator<(const Item& other) const {
    if (key_ != other.key_) {
      return key_ < other.key_;
    }
    return version_ > other.version_;
  }
  INLINE bool operator>=(const Item& other) const { return !(*this < other); }
  INLINE bool operator>(const Item& other) const { return other < *this; }
  INLINE bool operator<=(const Item& other) const { return !(*this > other); }

};  // struct Item

static_assert(sizeof(Item) == 8 + PAYLOAD, "sizeof(Item) must equal 8 + PAYLOAD");
static_assert(sizeof(Item) % 8 == 0, "Item size must be 8-byte aligned");

/// Domain size D for DOPartition. Configure before use.
inline uint64_t DOMAIN_SIZE_D = (1ULL << 40) - 1;

/// Maximum write count Nmax for SapphMap privacy budget planning. Configure before use.
inline uint64_t MAX_WRITES_N = (1ULL << 32) - 1;

}  // namespace EM::Definition

namespace EM::Sapphmap {

/**
 * @brief Generic partitioned data structure used by SapphMap levels.
 *
 * A PrivDS instance stores partition boundaries and per-partition counts,
 * plus a padded item array produced by DOItemAssign. The vector back-ends are
 * template parameters so the same algorithm can target enclave memory and
 * encrypted external memory without changing algorithmic logic.
 */

/// Partitioned data structure built from sorted items.
/// Stores DP-oblivious boundaries, per-segment counts, and padded item segments.
///
/// @tparam VecType        primary vector back-end (items, counts)
/// @tparam CachedVecType  boundary vector back-end (may have boundary cache)
/// @tparam IntmdVecType   vector back-end for scratch buffers (DOPMerge
///                        newitemsBuffer, DOPartition seal); defaults to
///                        VecType; set to an external-memory type when
///                        VecType is enclave-resident to avoid heap pressure
template <template <class, auto...> class VecType,
          template <class, auto...> class CachedVecType,
          template <class, auto...> class IntmdVecType = VecType>
struct PrivDS {
  using ItemVec   = VecType<EM::Definition::Item>;
  using BndVec    = CachedVecType<uint64_t>;
  using CountVec  = VecType<uint64_t>;

  template <typename T, auto...>
  using VecTemplate = VecType<T>;
  template <typename T, auto...>
  using CachedVecTemplate = CachedVecType<T>;

  std::unique_ptr<ItemVec> items;
  std::unique_ptr<BndVec> boundaries;
  std::unique_ptr<CountVec> counts;
  uint64_t Z = 0;  // Maximum padded items per segment.
  uint64_t Z_raw = 0;  // Unpadded segment length before block alignment.

  /// Returns recursive memory usage for this PrivDS object and owned vectors.
  uint64_t getByteSize() const {
      auto vecBytes = [](const auto& v) -> uint64_t {
          using Vec = std::decay_t<decltype(v)>;
          if constexpr (Vec::item_per_page == 1) {
              return sizeof(Vec) + v.capacity() * sizeof(typename Vec::value_type);
          } else if constexpr (!Vec::useStdCopy) {
              using Page = typename Vec::Page;
              using EncPage = typename Page::Encrypted_t;
              uint64_t num_pages = divRoundUp(v.N, Vec::item_per_page);
              return sizeof(Vec) + num_pages * sizeof(EncPage);
          } else {
              using Page = typename Vec::Page;
              using EncPage = typename Page::Encrypted_t;
              uint64_t num_pages = v.N / Vec::item_per_page + 1;
              return sizeof(Vec) + Vec::DMCacheSize * (sizeof(Page) + 16)
                     + num_pages * sizeof(EncPage);
          }
      };
      uint64_t bytes = sizeof(*this);
      if (items) bytes += vecBytes(*items);
      if (boundaries) bytes += vecBytes(*boundaries);
      if (counts) bytes += vecBytes(*counts);
      return bytes;
  }

  /// Build a fresh PDS from sorted items via DOPartition + DOItemAssign.
  template <typename InputVecType>
  void init(InputVecType& input, double eps_svt, double delta_svt,
            double eps_op, double delta_op, uint64_t cache_page_size = BLOCK_SIZE);

  /// Merge this PDS with `other` via DOPMerge, producing a new partitioned structure.
  /// The other PDS may use different vector back-ends (heterogeneous merge).
  template <template <class, auto...> class OtherVecType,
            template <class, auto...> class OtherCachedVecType,
            template <class, auto...> class OtherIntmdVecType>
  void reconstruct(PrivDS<OtherVecType, OtherCachedVecType, OtherIntmdVecType>& other,
                   double eps_svt, double delta_svt,
                   double eps_op, double delta_op,
                   uint64_t cache_page_size,
                   uint64_t total_real_count,
                   bool remove_deletions = false);

  /// Assign this->items from an IntmdVecType result produced by DOItemAssign.
  /// If IntmdVecType == VecType the result is moved directly.
  /// Otherwise, `source_to_release` is freed first (to reclaim VecType memory)
  /// and then the IntmdVecType data is copied into a fresh VecType allocation.
  template <typename SourceToRelease>
  void finalize_items(IntmdVecType<EM::Definition::Item>&& assigned_intmd,
                      SourceToRelease& source_to_release);
};

}  // namespace EM::Sapphmap


