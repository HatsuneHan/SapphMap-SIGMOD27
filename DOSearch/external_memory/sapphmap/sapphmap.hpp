#pragma once

/**
 * @file sapphmap.hpp
 * @brief Main SapphMap index implementation for SGX-oriented external memory.
 *
 * SapphMap combines an in-enclave mutable buffer with multi-level partitioned
 * runs (PrivDS). Upper levels can stay in enclave memory for low-latency
 * queries, while deeper levels are stored in encrypted external memory and
 * indexed by a static B+ tree over partition boundaries.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "common/utils.hpp"
#include "external_memory/sapphmap/pds_def.hpp"
#include "external_memory/sapphmap/privpartition.hpp"
#include "external_memory/tools/bplustree.hpp"
#include "external_memory/tools/probability_utils.hpp"

namespace EM::Sapphmap {

/**
 * @brief Plain key-value pair used by dataset loaders and bulk ingestion.
 *
 * The type is intentionally trivial so encrypted vector back-ends can copy and
 * serialize elements without custom constructors.
 */

/// Trivially copyable kv pair for use with Vector (encryption requires trivial types).
struct KvPair {
  uint64_t key;
  uint64_t value;
  bool operator<(const KvPair& o) const {
    return key < o.key || (key == o.key && value < o.value);
  }
};

/// SapphMap: external-memory structure with buffer + levels, PrivDS per level.
///
/// Top levels (1..enclave_levels) store PrivDS in enclave memory (StdVector)
/// for fast PointQuery / RangeQuery without encrypted page I/O.
/// Remaining levels use encrypted external memory (NCVec / BndCachedVec).
///
/// @tparam CachePageSize  block size for enclave <-> untrusted-memory transfers (bytes)
/// @tparam EnclaveSize    enclave memory budget (bytes)
template <uint64_t CachePageSize = BLOCK_SIZE, uint64_t EnclaveSize = ENCLAVE_SIZE>
struct SapphMap {
  using Item = EM::Definition::Item;
  using OpType = EM::Definition::OpType;

  // --- B+ tree page size: fixed at 4KB for boundary vectors ---
  static constexpr uint64_t kBPlusTreePageSize = 4096;

  // --- B+ tree node size aligned to one cache page ---
  static constexpr uint64_t kLevelBPlusTreeBlockSize =
      (kBPlusTreePageSize - 32) / sizeof(uint64_t);

  static constexpr uint64_t kBoundaryCacheSize = 1024;

  /// Overhead factor: accounts for PDS padding (~2x), metadata (boundaries,
  /// counts, B+ tree), and temp buffers during merge.
  /// buffer_real_count = buffer_size / (sizeof(Item) * kEnclaveOverheadFactor).
  static constexpr double kEnclaveOverheadFactor = 2.5;

  // --- Internal vector type aliases ---
  template <typename T, auto...>
  using NCVec = EM::NonCachedVector::Vector<T, CachePageSize - 32, true, true>;

  template <typename T, auto...>
  using BndCachedVec =
      EM::ExtVector::Vector<T, kBPlusTreePageSize - 32, true, true, kBoundaryCacheSize>;

  template <typename T, auto...>
  using StdVec = StdVector<T>;

  /**
   * @brief Range query container that abstracts enclave vs external output.
   */
  struct RangeQueryResult {
    bool std_active = true;
    std::unique_ptr<StdVec<Item>> std_vec;
    std::unique_ptr<NCVec<Item>> nc_vec;

    RangeQueryResult(StdVec<Item>&& v)
        : std_active(true), std_vec(std::make_unique<StdVec<Item>>(std::move(v))) {}
    RangeQueryResult(NCVec<Item>&& v)
        : std_active(false), nc_vec(std::make_unique<NCVec<Item>>(std::move(v))) {}

    bool is_stdvec() const { return std_active; }

    uint64_t size() const {
      return std_active
                 ? static_cast<uint64_t>(std_vec ? std_vec->size() : 0)
                 : static_cast<uint64_t>(nc_vec ? nc_vec->size() : 0);
    }

    StdVec<Item>& as_stdvec() {
      Assert(std_active && std_vec != nullptr);
      return *std_vec;
    }
    const StdVec<Item>& as_stdvec() const {
      Assert(std_active && std_vec != nullptr);
      return *std_vec;
    }
    NCVec<Item>& as_ncvec() {
      Assert(!std_active && nc_vec != nullptr);
      return *nc_vec;
    }
    const NCVec<Item>& as_ncvec() const {
      Assert(!std_active && nc_vec != nullptr);
      return *nc_vec;
    }
  };

  // --- Dual PrivDS types ---
  // ExternalPrivDS: items/counts in NCVec, boundaries in BndCachedVec.
  //   IntmdVecType defaults to NCVec (= VecType) — already external.
  // EnclavePrivDS: items/counts/boundaries in StdVec (enclave memory).
  //   IntmdVecType explicitly set to NCVec so scratch buffers (DOPMerge
  //   newitemsBuffer, DOPartition seal) go to external memory instead of
  //   consuming enclave heap.
  using ExternalPrivDS = PrivDS<NCVec, BndCachedVec>;
  using EnclavePrivDS  = PrivDS<StdVec, StdVec, NCVec>;

  /// Backward-compat alias (tests reference PrivDSType).
  using PrivDSType = ExternalPrivDS;

  using BPlusTreeType = StaticBPlusTree<BndCachedVec<uint64_t>>;

  /**
   * @brief Per-level storage state for one SapphMap run.
   */
  struct LevelState {
    bool occupied = false;
    bool is_enclave = false;
    uint64_t real_count = 0;
    uint64_t ncapacity = 0;
    std::unique_ptr<ExternalPrivDS> ext_pds;
    std::unique_ptr<BPlusTreeType> ext_bptree;
    std::unique_ptr<EnclavePrivDS> enc_pds;

    LevelState();
    void clear();

    uint64_t getByteSize() const {
        uint64_t bytes = sizeof(*this);
        bytes += buffer_size;
        if (ext_pds) bytes += ext_pds->getByteSize();
        if (ext_bptree) bytes += ext_bptree->getByteSize();
        if (enc_pds) bytes += enc_pds->getByteSize();
        return bytes;
    }
  };

  // --- Parameters and state ---
  uint64_t gamma;
  uint64_t buffer_size;      ///< memory budget in BYTES (e.g. 128MB)
  uint64_t buffer_real_count;  ///< max real items per unit = buffer_size / sizeof(Item) / kEnclaveOverheadFactor
  uint64_t depth;
  uint64_t enclave_levels;  ///< levels 1..enclave_levels stored in enclave
  
  double eps_total;
  double delta_total;

  double eps_svt;
  double delta_svt;

  double eps_op;
  double delta_op;

  double eps_init;
  double delta_init;

  StdVector<Item> buffer;
  StdVector<LevelState> levels;
  uint64_t next_ver_ = 0;

  bool static_mode_ = false;

  // --- Construction and configuration ---
  SapphMap(uint64_t _buffer_size, uint64_t _gamma, uint64_t _max_writes,
           double _eps_total, double _delta_total, bool _static_mode = false);

  void configure(uint64_t _buffer_size, uint64_t _gamma, uint64_t _max_writes,
                 double _eps_total, double _delta_total, bool _static_mode = false);

  // --- Low-level write (used by tests and internally) ---
  void write(const Item& x);
  void write(OpType op, uint64_t key, uint64_t value, uint64_t ver);

  // --- High-level API (version maintained internally) ---
  void Insert(uint64_t key, uint64_t value);
  void Delete(uint64_t key);
  template <typename InputVec>
  void bulk_load(InputVec& input);
  template <typename DatasetVec>
  void Ingest(DatasetVec& dataset);

  // --- Queries ---
  Item PointQuery(uint64_t key);
  RangeQueryResult RangeQuery(uint64_t l, uint64_t r);
  uint64_t level_count() const;

  // --- Level access ---
  bool level_occupied(uint64_t l) const;
  bool level_in_enclave(uint64_t l) const;
  const ExternalPrivDS* level_ext_pds(uint64_t l) const;
  const EnclavePrivDS* level_enc_pds(uint64_t l) const;
  const BPlusTreeType* level_bptree(uint64_t l) const;

  /// Backward compat: returns ExternalPrivDS* (nullptr for enclave levels).
  const PrivDSType* level_pds(uint64_t l) const;

  /// Visit level's PDS with a generic lambda: func(const auto& pds).
  template <typename Func>
  void visit_level_pds(uint64_t l, Func&& func) const;

  double getByteSize() const {
      uint64_t bytes = sizeof(*this);
      bytes += buffer.capacity() * sizeof(Item);
      bytes += levels.capacity() * sizeof(LevelState);
      double total = static_cast<double>(bytes);
      for (size_t i = 0; i < levels.size(); ++i) {
          const LevelState& ls = levels[i];
          if (ls.ext_pds) total += ls.ext_pds->getByteSize();
          if (ls.ext_bptree) total += ls.ext_bptree->getByteSize();
          if (ls.enc_pds) total += ls.enc_pds->getByteSize();
      }
      return std::round(total * 100.0) / 100.0;
  }

  /// Space owned by the per-level external B+ tree indexes. The referenced
  /// boundary (leaf) vectors are owned and accounted for by PrivDS, so they
  /// are intentionally excluded here to avoid double counting.
  uint64_t getBPlusTreeByteSize() const {
      uint64_t bytes = 0;
      for (size_t i = 0; i < levels.size(); ++i) {
          const LevelState& ls = levels[i];
          if (ls.ext_bptree) bytes += ls.ext_bptree->getByteSize();
      }
      return bytes;
  }

 private:
  void rebuild_level_bptree(LevelState& level);

  /// Runtime-variant carry used during level cascade in place_run / bulk_load.
  struct PDSCarry {
    std::unique_ptr<ExternalPrivDS> ext;
    std::unique_ptr<EnclavePrivDS> enc;
    bool is_enclave() const { return enc != nullptr; }
  };

  void place_run(PDSCarry carry, uint64_t start_level,
                 uint64_t real_count_incoming);

  /// Generic PDS conversion: copy items/boundaries/counts from SourcePDS to
  /// a newly created TargetPDS.  Works for any combination of ExternalPrivDS
  /// and EnclavePrivDS.
  template <typename TargetPDS, typename SourcePDS>
  static std::unique_ptr<TargetPDS> convert_pds(SourcePDS& src);
};

// ============== Implementation ==============

// --- LevelState ---

template <uint64_t CachePageSize, uint64_t EnclaveSize>
INLINE SapphMap<CachePageSize, EnclaveSize>::LevelState::LevelState() {}

template <uint64_t CachePageSize, uint64_t EnclaveSize>
INLINE void SapphMap<CachePageSize, EnclaveSize>::LevelState::clear() {
  occupied = false;
  is_enclave = false;
  real_count = 0;
  ext_bptree.reset();
  ext_pds.reset();
  enc_pds.reset();
}

// --- Constructor / configure ---

template <uint64_t CachePageSize, uint64_t EnclaveSize>
INLINE SapphMap<CachePageSize, EnclaveSize>::SapphMap(
    uint64_t _buffer_size, uint64_t _gamma, uint64_t _max_writes,
    double _eps_total, double _delta_total, bool _static_mode) {
  configure(_buffer_size, _gamma, _max_writes, _eps_total, _delta_total, _static_mode);
}

template <uint64_t CachePageSize, uint64_t EnclaveSize>
/**
 * @brief Configure capacity and privacy-budget splits for one SapphMap instance.
 *
 * The method derives level capacities from buffer and fanout parameters, then
 * distributes the global $(\epsilon,\delta)$ budget across partitioning and
 * assignment phases under either static build mode or dynamic update mode.
 */
inline void SapphMap<CachePageSize, EnclaveSize>::configure(
    uint64_t _buffer_size, uint64_t _gamma, uint64_t _max_writes,
    double _eps_total, double _delta_total, bool _static_mode) {
  buffer_size = _buffer_size;
  gamma = _gamma;
  eps_total = _eps_total;
  delta_total = _delta_total;

  // Derive max real items per "unit" from byte budget.
  buffer_real_count = static_cast<uint64_t>(buffer_size /
      (sizeof(Item) * kEnclaveOverheadFactor));
  if (buffer_real_count == 0) buffer_real_count = 1;

  const double level_log =
      obliLog(static_cast<double>(_max_writes) /
              static_cast<double>(buffer_real_count)) /
      obliLog(static_cast<double>(gamma));
  const uint64_t merge_levels = obliCeilU64(level_log);
  const uint64_t max_level = obliFloorU64(level_log) + 1;
  const uint64_t mu = (gamma - 1) * merge_levels + 1;

  if (_static_mode) {
    // Static (bulk-load-only) mode: a single Ingest() calls init() once per
    // level, consuming eps_svt (SVT boundaries) + 1 * eps_init (DOItemAssign
    // inside merge paths).  Splitting the total budget into
    // 4 equal parts covers the full budget exactly.
    static_mode_ = true;
    eps_svt   = 0.9 * eps_total;
    delta_svt = 0.9 * delta_total;
    eps_op    = 0;          // fallback; not exercised in static build
    delta_op  = 0;
    eps_init  = 0.1 * eps_total;
    delta_init = 0.1 * delta_total;
  } else {
    // Dynamic mode: split privacy budget across mu merge invocations.
    // 0.75 for SVT boundaries, 0.24 for ops (2*mu: DOPMerge+DOItemAssign),
    // 0.01 for initialization.
    static_mode_ = false;
    auto [eps_svt_comp, delta_svt_comp] =
        basic_composition(0.75 * eps_total, 0.75 * delta_total, mu);
    auto [eps_op_comp, delta_op_comp] =
        basic_composition(0.24 * eps_total, 0.24 * delta_total, 2 * mu);

    eps_svt = eps_svt_comp;
    delta_svt = delta_svt_comp;
    eps_op = eps_op_comp;
    delta_op = delta_op_comp;

    eps_init = 0.01 * eps_total;
    delta_init = 0.01 * delta_total;
  }
  (void)mu;  // used above; suppress unused-variable warning in static mode

  depth = 0;
  next_ver_ = 0;
  buffer.clear();
  buffer.reserve(buffer_real_count);
  levels.clear();
  levels.resize(max_level + 1);
  uint64_t cap = buffer_real_count;
  for (uint64_t i = 0; i <= max_level; ++i) {
    levels[i].ncapacity = cap;
    cap *= gamma;
  }

  // Estimate how many levels can remain enclave-resident.
  // One unit equals buffer_size bytes; level i costs approximately gamma^i units.
  // Temporary vectors used by DOPMerge/DOPartition are externalized and excluded.
  const uint64_t initial_units = (buffer_size > 0) ? (EnclaveSize / buffer_size) : 0;
  const uint64_t total_units = (initial_units > 1) ? (initial_units - 1) : 1;
  
  enclave_levels = 0;
  uint64_t used_units = 1;  // buffer
  uint64_t gamma_pow = gamma;
  for (uint64_t i = 1; i <= max_level; ++i) {
    if (used_units + gamma_pow > total_units) break;
    used_units += gamma_pow;
    enclave_levels = i;
    gamma_pow *= gamma;
  }
}

// --- convert_pds ---

template <uint64_t CachePageSize, uint64_t EnclaveSize>
template <typename TargetPDS, typename SourcePDS>
inline std::unique_ptr<TargetPDS>
SapphMap<CachePageSize, EnclaveSize>::convert_pds(SourcePDS& src) {
  auto result = std::make_unique<TargetPDS>();
  result->Z = src.Z;
  result->Z_raw = src.Z_raw;

  using TargetItemVec = typename TargetPDS::ItemVec;
  using TargetBndVec  = typename TargetPDS::BndVec;
  using TargetCountVec = typename TargetPDS::CountVec;
  using SourceItemVec = typename SourcePDS::ItemVec;
  using SourceBndVec  = typename SourcePDS::BndVec;
  using SourceCountVec = typename SourcePDS::CountVec;

  if (src.items) {
    const uint64_t n = src.items->size();
    result->items = std::make_unique<TargetItemVec>(n);
    typename SourceItemVec::Reader reader(src.items->begin(), src.items->end());
    typename TargetItemVec::Writer writer(result->items->begin(), result->items->end());
    for (uint64_t i = 0; i < n; ++i) writer.write(reader.read());
    writer.flush();
  }

  if (src.boundaries) {
    const uint64_t n = src.boundaries->size();
    result->boundaries = std::make_unique<TargetBndVec>(n);
    typename SourceBndVec::Reader reader(src.boundaries->begin(), src.boundaries->end());
    typename TargetBndVec::Writer writer(result->boundaries->begin(), result->boundaries->end());
    for (uint64_t i = 0; i < n; ++i) writer.write(reader.read());
    writer.flush();
  }

  if (src.counts) {
    const uint64_t n = src.counts->size();
    result->counts = std::make_unique<TargetCountVec>(n);
    typename SourceCountVec::Reader reader(src.counts->begin(), src.counts->end());
    typename TargetCountVec::Writer writer(result->counts->begin(), result->counts->end());
    for (uint64_t i = 0; i < n; ++i) writer.write(reader.read());
    writer.flush();
  }

  return result;
}

// --- write ---

template <uint64_t CachePageSize, uint64_t EnclaveSize>
inline void SapphMap<CachePageSize, EnclaveSize>::write(const Item& x) {
  buffer.push_back(x);
  if (buffer.size() >= buffer_real_count) {
    PDSCarry carry;
    if (enclave_levels >= 1) {
      auto run = std::make_unique<EnclavePrivDS>();
      run->init(buffer, eps_svt, delta_svt, eps_op, delta_op, CachePageSize);
      carry.enc = std::move(run);
    } else {
      auto run = std::make_unique<ExternalPrivDS>();
      run->init(buffer, eps_svt, delta_svt, eps_op, delta_op, CachePageSize);
      carry.ext = std::move(run);
    }
    buffer.clear();
    place_run(std::move(carry), 1, buffer_real_count);
  }
}

template <uint64_t CachePageSize, uint64_t EnclaveSize>
inline void SapphMap<CachePageSize, EnclaveSize>::write(
    OpType op, uint64_t key, uint64_t value, uint64_t ver) {
  write(Item(op, key, value, ver));
}

template <uint64_t CachePageSize, uint64_t EnclaveSize>
inline void SapphMap<CachePageSize, EnclaveSize>::Insert(uint64_t key, uint64_t value) {
  ++next_ver_;
  write(OpType::INS, key, value, next_ver_);
}

template <uint64_t CachePageSize, uint64_t EnclaveSize>
inline void SapphMap<CachePageSize, EnclaveSize>::Delete(uint64_t key) {
  ++next_ver_;
  write(OpType::DEL, key, 0, next_ver_);
}

// --- Ingest ---

template <uint64_t CachePageSize, uint64_t EnclaveSize>
template <typename DatasetVec>
inline void SapphMap<CachePageSize, EnclaveSize>::Ingest(DatasetVec& dataset) {
  if (dataset.size() == 0) return;

  ++next_ver_;
  const uint64_t ver = next_ver_;

  NCVec<Item> input(dataset.size());
  typename NCVec<Item>::Writer writer(input.begin(), input.end());
  typename DatasetVec::Reader reader(dataset.begin(), dataset.end());
  for (uint64_t i = 0; i < dataset.size(); ++i) {
    KvPair kv = reader.read();
    writer.write(Item(OpType::INS, kv.key, kv.value, ver));
  }
  writer.flush();
  bulk_load(input);
}

// --- bulk_load ---

template <uint64_t CachePageSize, uint64_t EnclaveSize>
template <typename InputVec>
/**
 * @brief Bulk-load a sorted or prebuilt run and cascade merges as needed.
 *
 * This path is used by benchmark ingestion and preserves the same merge logic
 * as online writes, while allowing a direct jump to the absorbing level.
 */
inline void SapphMap<CachePageSize, EnclaveSize>::bulk_load(InputVec& input) {
  if (input.size() == 0) return;

  const uint64_t buf_size = buffer.size();
  const uint64_t inp_size = input.size();
  const uint64_t base_total = buf_size + inp_size;

  if (base_total < buffer_real_count) {
    typename InputVec::Reader inputReader(input.begin(), input.end());
    for (uint64_t i = 0; i < inp_size; ++i)
      buffer.push_back(inputReader.read());
    return;
  }

  // Find the first level that can absorb current input plus occupied lower levels.
  uint64_t accumulated = base_total;
  uint64_t target = 1;
  while (accumulated > levels[target].ncapacity) {
    if (levels[target].occupied)
      accumulated += levels[target].real_count;
    ++target;
  }

  const bool target_is_enclave = (target <= enclave_levels);

  // Phase 1: merge levels 1..target-1 bottom-up.
  // When target is enclave all absorbed levels are also enclave → keep carry.enc.
  // When target is external, accumulate as ExternalPrivDS.
  PDSCarry level_carry;
  uint64_t level_records = 0;
  for (uint64_t j = 1; j < target; ++j) {
    if (!levels[j].occupied) continue;
    uint64_t j_records = levels[j].real_count;
    level_records += j_records;
    if (!level_carry.ext && !level_carry.enc) {
      // First absorbed level: take PDS directly.
      if (levels[j].is_enclave) {
        level_carry.enc = std::move(levels[j].enc_pds);
      } else {
        level_carry.ext = std::move(levels[j].ext_pds);
      }
    } else if (target_is_enclave) {
      // All absorbed levels are enclave; accumulate as EnclavePrivDS.
      level_carry.enc->template reconstruct(*levels[j].enc_pds, eps_svt, delta_svt, eps_op, delta_op, CachePageSize, level_records, false);
    } else {
      // External target: accumulate as ExternalPrivDS.
      if (!level_carry.ext) {
        level_carry.ext = convert_pds<ExternalPrivDS>(*level_carry.enc);
        level_carry.enc.reset();
      }
      if (levels[j].is_enclave) {
        level_carry.ext->template reconstruct(*levels[j].enc_pds, eps_svt, delta_svt, eps_op, delta_op, CachePageSize, level_records, false);
      } else {
        level_carry.ext->template reconstruct(*levels[j].ext_pds, eps_svt, delta_svt, eps_op, delta_op, CachePageSize, level_records, false);
      }
    }
    levels[j].clear();
  }

  // Phase 2+3: build PDS from buffer+input (matching target type), then merge
  // with absorbed levels carry if present.
  const uint64_t total_records = base_total + level_records;
  PDSCarry final_carry;

  if (target_is_enclave) {
    auto enc_run = std::make_unique<EnclavePrivDS>();
    if (buf_size == 0) { 
      if(level_carry.enc == nullptr && level_carry.ext == nullptr){
        enc_run->init(input, eps_svt, delta_svt, eps_init, delta_init, CachePageSize);
      } else {
        enc_run->init(input, eps_svt, delta_svt, eps_op, delta_op, CachePageSize);
      }
    } else {
      StdVector<Item> combined(base_total);
      typename StdVector<Item>::Writer writer(combined.begin(), combined.end());
      for (const auto& x : buffer) writer.write(x);
      typename InputVec::Reader inputReader(input.begin(), input.end());
      for (uint64_t i = 0; i < inp_size; ++i) writer.write(inputReader.read());
      writer.flush();
      enc_run->init(combined, eps_svt, delta_svt, eps_op, delta_op, CachePageSize);
    }
    buffer.clear();
    if (level_carry.enc) {
      enc_run->template reconstruct(*level_carry.enc, eps_svt, delta_svt, eps_op, delta_op, CachePageSize, total_records, false);
    } else if (level_carry.ext) {
      enc_run->template reconstruct(*level_carry.ext, eps_svt, delta_svt, eps_op, delta_op, CachePageSize, total_records, false);
    }
    final_carry.enc = std::move(enc_run);
  } else {
    auto ext_run = std::make_unique<ExternalPrivDS>();
    if (buf_size == 0) {
      if(level_carry.enc == nullptr && level_carry.ext == nullptr){
        ext_run->init(input, eps_svt, delta_svt, eps_init, delta_init, CachePageSize);
      } else {
        ext_run->init(input, eps_svt, delta_svt, eps_op, delta_op, CachePageSize);
      }
    } else {
      NCVec<Item> combined(base_total);
      typename NCVec<Item>::Writer writer(combined.begin(), combined.end());
      for (const auto& x : buffer) writer.write(x);
      typename InputVec::Reader inputReader(input.begin(), input.end());
      for (uint64_t i = 0; i < inp_size; ++i) writer.write(inputReader.read());
      writer.flush();
      ext_run->init(combined, eps_svt, delta_svt, eps_op, delta_op, CachePageSize);
    }
    buffer.clear();
    if (level_carry.enc) {
      ext_run->template reconstruct(*level_carry.enc, eps_svt, delta_svt, eps_op, delta_op, CachePageSize, total_records, false);
    } else if (level_carry.ext) {
      ext_run->template reconstruct(*level_carry.ext, eps_svt, delta_svt, eps_op, delta_op, CachePageSize, total_records, false);
    }
    final_carry.ext = std::move(ext_run);
  }

  // Phase 4: place at target level.
  place_run(std::move(final_carry), target, total_records);
}

// --- place_run ---

template <uint64_t CachePageSize, uint64_t EnclaveSize>
/**
 * @brief Place a run at or above @p start_level, cascading on capacity overflow.
 */
inline void SapphMap<CachePageSize, EnclaveSize>::place_run(
    PDSCarry carry, uint64_t start_level, uint64_t real_count_incoming) {
  uint64_t level = start_level;
  uint64_t incoming_real = real_count_incoming;

  while (true) {
    LevelState& dst = levels[level];

    if (!dst.occupied) {
      dst.occupied = true;
      dst.real_count = incoming_real;
      if (level <= enclave_levels) {
        dst.is_enclave = true;
        // Carry is guaranteed enc at enclave levels (invariant maintained by
        // write() and bulk_load()).
        dst.enc_pds = std::move(carry.enc);
        dst.ext_pds.reset();
        dst.ext_bptree.reset();
      } else {
        dst.is_enclave = false;
        if (carry.is_enclave()) {
          // Transition from enclave cascade into empty external slot.
          dst.ext_pds = convert_pds<ExternalPrivDS>(*carry.enc);
        } else {
          dst.ext_pds = std::move(carry.ext);
        }
        dst.enc_pds.reset();
        rebuild_level_bptree(dst);
      }
      depth = obliMax(depth, level);
      return;
    }

    // Merge carry into existing level's PDS using heterogeneous reconstruct.
    // The result type matches the level's PDS type (enclave or external).
    const uint64_t merged_real = dst.real_count + incoming_real;
    const bool remove_del = (level == depth);

    if (dst.is_enclave) {
      if (carry.is_enclave()) {
        dst.enc_pds->template reconstruct(*carry.enc, eps_svt, delta_svt, eps_op, delta_op, CachePageSize, merged_real, remove_del);
      } else {
        dst.enc_pds->template reconstruct(*carry.ext, eps_svt, delta_svt, eps_op, delta_op, CachePageSize, merged_real, remove_del);
      }
    } else {
      if (carry.is_enclave()) {
        dst.ext_pds->template reconstruct(*carry.enc, eps_svt, delta_svt, eps_op, delta_op, CachePageSize, merged_real, remove_del);
      } else {
        dst.ext_pds->template reconstruct(*carry.ext, eps_svt, delta_svt, eps_op, delta_op, CachePageSize, merged_real, remove_del);
      }
    }

    if (merged_real < dst.ncapacity) {
      dst.real_count = merged_real;
      if (dst.is_enclave) {
        dst.ext_pds.reset();
        dst.ext_bptree.reset();
      } else {
        dst.enc_pds.reset();
        rebuild_level_bptree(dst);
      }
      return;
    }

    // Carry the merged result forward to the next level.
    carry.ext.reset();
    carry.enc.reset();
    if (dst.is_enclave) {
      carry.enc = std::move(dst.enc_pds);
    } else {
      carry.ext = std::move(dst.ext_pds);
    }
    incoming_real = merged_real;
    dst.clear();
    ++level;
  }
}

// --- PointQuery ---

template <uint64_t CachePageSize, uint64_t EnclaveSize>
/**
 * @brief Point lookup over buffer and levels with latest-version resolution.
 */
inline typename SapphMap<CachePageSize, EnclaveSize>::Item
SapphMap<CachePageSize, EnclaveSize>::PointQuery(uint64_t key) {
  Item best_item;
  bool has_result = false;

  // Scan in-memory buffer first to preserve newest-write precedence.
  for (const Item& x : buffer) {
    const bool match =
        (x.get_key() == key) & (!x.is_dummy()) &
        (!has_result | (x.get_ver() > best_item.get_ver()));
    obliMove(match, best_item, x);
    obliMove(match, has_result, true);
  }
  for (uint64_t i = (static_mode_ ? depth : 1); i <= depth; ++i) {
    if (!level_occupied(i)) continue;
    const LevelState& ls = levels[i];

    if (ls.is_enclave) {
      // Enclave level: binary search on StdVector boundaries, direct item access.
      const EnclavePrivDS* enc = ls.enc_pds.get();
      if (!enc || !enc->items || enc->Z == 0) continue;

      const uint64_t j =
          (enc->boundaries && enc->boundaries->size() > 0)
              ? boundary_lower_bound(*enc->boundaries, key)
              : 0;

      const uint64_t offset = j * enc->Z;
      if (offset + enc->Z > enc->items->size()) continue;
      for (uint64_t k = 0; k < enc->Z; ++k) {
        const Item& item = (*enc->items)[offset + k];
        const bool match =
            (item.get_key() == key) & (!item.is_dummy()) &
            (!has_result | (item.get_ver() > best_item.get_ver()));
        obliMove(match, best_item, item);
        obliMove(match, has_result, true);
      }
    } else {
      // External level: B+ tree lookup + encrypted segment read.
      const ExternalPrivDS* pds = ls.ext_pds.get();
      const BPlusTreeType* bt = ls.ext_bptree.get();
      if (!pds || !bt || !pds->items || pds->Z == 0) continue;

      BPlusTreeQueryResult qr =
          lower_bound_query(const_cast<BPlusTreeType&>(*bt), key);
      if (qr.index == UINT64_MAX) continue;
      const uint64_t j = qr.index;
      const uint64_t offset = j * pds->Z;
      if (offset + pds->Z > pds->items->size()) continue;

      typename NCVec<Item>::Reader seg_reader(
          pds->items->begin() + offset,
          pds->items->begin() + offset + pds->Z);
      for (uint64_t k = 0; k < pds->Z; ++k) {
        Item item = seg_reader.read();
        const bool match =
            (item.get_key() == key) & (!item.is_dummy()) &
            (!has_result | (item.get_ver() > best_item.get_ver()));
        obliMove(match, best_item, item);
        obliMove(match, has_result, true);
      }
    }
  }

  const Item dummy_item;
  obliMove(!has_result, best_item, dummy_item);
  obliMove(best_item.get_op() == OpType::DEL, best_item, dummy_item);
  return best_item;
}

// --- RangeQuery ---

template <uint64_t CachePageSize, uint64_t EnclaveSize>
/**
 * @brief Range lookup that materializes candidate segments from all occupied levels.
 */
inline auto SapphMap<CachePageSize, EnclaveSize>::RangeQuery(uint64_t l, uint64_t r)
    -> RangeQueryResult {
  struct LevelRangeInfo {
    uint64_t level;
    uint64_t seg_start;
    uint64_t num_segments;
    bool is_enc;
  };
  uint64_t count = 0;
  std::vector<LevelRangeInfo> level_infos;

  for (uint64_t i = (static_mode_ ? depth : 1); i <= depth; ++i) {
    if (!level_occupied(i)) continue;
    const LevelState& ls = levels[i];

    if (ls.is_enclave) {
      const EnclavePrivDS* enc = ls.enc_pds.get();
      if (!enc || !enc->items || !enc->boundaries || enc->Z == 0)
        continue;

      const uint64_t m = enc->boundaries->size();
      uint64_t j = boundary_lower_bound(*enc->boundaries, l);
      const uint64_t j_start = j;
      uint64_t num_seg = 0;
      while (j <= m) {
        if (j * enc->Z + enc->Z > enc->items->size()) break;
        count += enc->Z;
        ++num_seg;
        if (j < m) {
          if ((*enc->boundaries)[j] >= r) break;
        }
        ++j;
      }
      level_infos.push_back({i, j_start, num_seg, true});
    } else {
      const ExternalPrivDS* pds = ls.ext_pds.get();
      const BPlusTreeType* bt = ls.ext_bptree.get();
      if (!pds || !bt || !pds->items || !pds->boundaries || pds->Z == 0)
        continue;

      BPlusTreeQueryResult qr = lower_bound_query(const_cast<BPlusTreeType&>(*bt), l);
      if (qr.index == UINT64_MAX) continue;
      const uint64_t m = pds->boundaries->size();
      uint64_t j = qr.index;
      const uint64_t j_start = j;
      uint64_t num_seg = 0;
      typename BndCachedVec<uint64_t>::Reader boundary_reader(
          pds->boundaries->begin() + j, pds->boundaries->end());
      while (j <= m) {
        if (j * pds->Z + pds->Z > pds->items->size()) break;
        count += pds->Z;
        ++num_seg;
        if (j < m) {
          uint64_t s_j = boundary_reader.read();
          if (s_j >= r) break;
        }
        ++j;
      }
      level_infos.push_back({i, j_start, num_seg, false});
    }
  }

  // Buffer contributes directly to output length.
  count += buffer.size();

  auto fill_output = [&](auto& output) {
    using OutVec = std::decay_t<decltype(output)>;
    typename OutVec::Writer writer(output.begin(), output.end());

    // Emit buffered items first.
    for (const Item& x : buffer)
      writer.write(x);

    for (const LevelRangeInfo& info : level_infos) {
      if (info.is_enc) {
        const EnclavePrivDS* enc = levels[info.level].enc_pds.get();
        if (!enc || !enc->items || enc->Z == 0) continue;
        const uint64_t offset = info.seg_start * enc->Z;
        const uint64_t n_read = info.num_segments * enc->Z;
        if (offset + n_read > enc->items->size()) continue;
        for (uint64_t k = 0; k < n_read; ++k)
          writer.write((*enc->items)[offset + k]);
      } else {
        const ExternalPrivDS* pds = levels[info.level].ext_pds.get();
        if (!pds || !pds->items || pds->Z == 0) continue;
        const uint64_t offset = info.seg_start * pds->Z;
        const uint64_t n_read = info.num_segments * pds->Z;
        if (offset + n_read > pds->items->size()) continue;
        typename NCVec<Item>::Reader reader(
            pds->items->begin() + offset,
            pds->items->begin() + offset + n_read);
        for (uint64_t k = 0; k < n_read; ++k)
          writer.write(reader.read());
      }
    }

    writer.flush();
  };

  if (enclave_levels >= depth) {
    StdVec<Item> output(count);
    fill_output(output);
    return output;
  }

  NCVec<Item> output(count);
  fill_output(output);
  return output;
}

// --- Level accessors ---

template <uint64_t CachePageSize, uint64_t EnclaveSize>
inline uint64_t SapphMap<CachePageSize, EnclaveSize>::level_count() const {
  return depth;
}

template <uint64_t CachePageSize, uint64_t EnclaveSize>
INLINE bool SapphMap<CachePageSize, EnclaveSize>::level_occupied(uint64_t l) const {
  return l < levels.size() && levels[l].occupied;
}

template <uint64_t CachePageSize, uint64_t EnclaveSize>
INLINE bool SapphMap<CachePageSize, EnclaveSize>::level_in_enclave(uint64_t l) const {
  return l < levels.size() && levels[l].occupied && levels[l].is_enclave;
}

template <uint64_t CachePageSize, uint64_t EnclaveSize>
INLINE const typename SapphMap<CachePageSize, EnclaveSize>::ExternalPrivDS*
SapphMap<CachePageSize, EnclaveSize>::level_ext_pds(uint64_t l) const {
  if (l >= levels.size() || !levels[l].occupied || levels[l].is_enclave)
    return nullptr;
  return levels[l].ext_pds.get();
}

template <uint64_t CachePageSize, uint64_t EnclaveSize>
INLINE const typename SapphMap<CachePageSize, EnclaveSize>::EnclavePrivDS*
SapphMap<CachePageSize, EnclaveSize>::level_enc_pds(uint64_t l) const {
  if (l >= levels.size() || !levels[l].occupied || !levels[l].is_enclave)
    return nullptr;
  return levels[l].enc_pds.get();
}

template <uint64_t CachePageSize, uint64_t EnclaveSize>
INLINE const typename SapphMap<CachePageSize, EnclaveSize>::PrivDSType*
SapphMap<CachePageSize, EnclaveSize>::level_pds(uint64_t l) const {
  return level_ext_pds(l);
}

template <uint64_t CachePageSize, uint64_t EnclaveSize>
INLINE const typename SapphMap<CachePageSize, EnclaveSize>::BPlusTreeType*
SapphMap<CachePageSize, EnclaveSize>::level_bptree(uint64_t l) const {
  if (l >= levels.size() || !levels[l].occupied || levels[l].is_enclave)
    return nullptr;
  return levels[l].ext_bptree.get();
}

template <uint64_t CachePageSize, uint64_t EnclaveSize>
template <typename Func>
void SapphMap<CachePageSize, EnclaveSize>::visit_level_pds(uint64_t l, Func&& func) const {
  if (l >= levels.size() || !levels[l].occupied) return;
  if (levels[l].is_enclave) {
    func(*levels[l].enc_pds);
  } else {
    func(*levels[l].ext_pds);
  }
}

template <uint64_t CachePageSize, uint64_t EnclaveSize>
inline void SapphMap<CachePageSize, EnclaveSize>::rebuild_level_bptree(LevelState& level) {
  level.ext_bptree.reset();
  if (!level.occupied || level.is_enclave) return;
  if (!level.ext_pds || !level.ext_pds->boundaries) return;
  level.ext_bptree = std::make_unique<BPlusTreeType>(
      build_bplus_tree<BndCachedVec>(*level.ext_pds->boundaries,
                                     kLevelBPlusTreeBlockSize,
                                     EM::Definition::DOMAIN_SIZE_D));
}

}  // namespace EM::Sapphmap
