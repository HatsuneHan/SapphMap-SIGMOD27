#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <list>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/defs.hpp"
#include "common/dmcache.hpp"
#include "common/tracing/perf.hpp"

#define USE_LRU_FLAG false

namespace CACHE {
enum CacheType {
  LRU,
  DM  // direct map
};

template <bool VERSIONED>
struct CacheVersionState {
  explicit CacheVersionState(uint64_t /*initialSize*/) {}
};

template <>
struct CacheVersionState<true> {
  std::vector<uint32_t> versions;

  explicit CacheVersionState(uint64_t initialSize)
      : versions(initialSize, 0) {}
};

template <typename T, typename S, uint64_t CACHE_SIZE = SERVER__CACHE_SIZE,
          uint64_t TLB_SIZE = 2, bool VERSIONED = false>
struct Cached : S, CacheVersionState<VERSIONED> {
  using Cache = typename std::conditional<USE_LRU_FLAG,
                                          CACHE::Cache<T, CACHE_SIZE, TLB_SIZE>,
                                          CACHE::DMCache<T, CACHE_SIZE> >::type;
  Cache cache;
  using typename S::IndexType;
  using BackendType = typename S::BackendType;

  void InitializeVersionedCache() {
    if constexpr (VERSIONED) {
      const IndexType invalid = std::numeric_limits<IndexType>::max();
      for (auto& entry : cache.data) {
        entry.idx = invalid;
        entry.dirty = false;
      }
    }
  }

  Cached(BackendType& backend, uint64_t initialSize, const T& defaultVal)
      : S(backend, initialSize, defaultVal),
        CacheVersionState<VERSIONED>(initialSize) {
    InitializeVersionedCache();
  }

  Cached(BackendType& backend, uint64_t initialSize)
      : S(backend, initialSize),
        CacheVersionState<VERSIONED>(initialSize) {
    InitializeVersionedCache();
  }

  Cached(Cached&& other) noexcept
      : S(other),
        CacheVersionState<VERSIONED>(
            std::move(static_cast<CacheVersionState<VERSIONED>&>(other))),
        cache(std::move(other.cache)) {}

  template <bool dirty = true, bool skip_read = false, bool writeBack = true>
  T& AccessLRU(const IndexType i) {
    PERFCTR_INCREMENT(accessCount);
    T* TLB_result = cache.AccessTLB(i, dirty, writeBack);
    if (TLB_result) {
      PERFCTR_INCREMENT(tlbHitCount);
      return *TLB_result;
    }

    if (!cache.CheckContains(i)) {
      if (cache.IsFull()) {
        IndexType evictedIndex;
        {
          auto& evicted = cache.GetNextToEvict(evictedIndex);
          if (evicted.dirty) {
            Write(evictedIndex, evicted.val);
          }
        }
        cache.EvictLRU(evictedIndex);
      }

      T ret;
      if (!skip_read) {
        Read(i, ret);
      }
      cache.Insert(i, ret);
    }

    return cache.Access(i, dirty, writeBack);
  }

  template <bool dirty = true, bool skip_read = false, bool writeBack = true>
  T& AccessDM(const IndexType i) {
    PERFCTR_INCREMENT(accessCount);
    auto& mappedSlot = cache.GetMappedSlot(i);
    if (mappedSlot.idx != i) {
      if (mappedSlot.dirty) {
        Write(mappedSlot.idx, mappedSlot.val);
      }
      if constexpr (!skip_read) {
        Read(i, mappedSlot.val);
        mappedSlot.dirty = false;
      }
      mappedSlot.idx = i;
    }
    mappedSlot.dirty = (mappedSlot.dirty || dirty) && writeBack;
    return mappedSlot.val;
  }

  template <bool dirty = true, bool skip_read = false, bool writeBack = true>
  T& Access(const IndexType i) {
    if constexpr (USE_LRU_FLAG) {
      return AccessLRU<dirty, skip_read, writeBack>(i);
    } else {
      return AccessDM<dirty, skip_read, writeBack>(i);
    }
  }

  const T& AccessReadOnly(const IndexType i) { return Access<false>(i); }

  const T& AccessNoWriteBack(const IndexType i) {
    return Access<false, false, false>(i);
  }

  T& AccessWriteOnly(const IndexType i) { return Access<true, true>(i); }

  void Write(const IndexType i, const T& in) {
    if constexpr (VERSIONED) {
      Assert(i < this->versions.size());
      // Never reuse an authenticated-encryption nonce for this page.
      if (this->versions[i] == UINT32_MAX) std::abort();
      S::Write(i, in, ++this->versions[i]);
    } else {
      S::Write(i, in);
    }
  }

  void Read(const IndexType i, T& out) {
    if constexpr (VERSIONED) {
      Assert(i < this->versions.size());
      S::Read(i, out, this->versions[i]);
    } else {
      S::Read(i, out);
    }
  }

  /** Write every dirty direct-mapped cache line and optionally invalidate it. */
  void FlushCache(bool invalidate = false) {
    static_assert(!USE_LRU_FLAG,
                  "FlushCache currently supports the configured DM cache");
    const IndexType invalid = std::numeric_limits<IndexType>::max();
    for (auto& entry : cache.data) {
      if (entry.idx != invalid && entry.dirty) {
        Write(entry.idx, entry.val);
        entry.dirty = false;
      }
      if (invalidate) {
        entry.idx = invalid;
        entry.dirty = false;
      }
    }
  }
};
}  // namespace CACHE