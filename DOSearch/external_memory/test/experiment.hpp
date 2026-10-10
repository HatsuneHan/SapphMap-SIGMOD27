#pragma once

#include "../../../applications/sapphmap/Enclave/Enclave.h"
#include "../../../applications/sapphmap/Enclave/Enclave_t.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string_view>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include "external_memory/sapphmap/pds_def.hpp"
#include "external_memory/sapphmap/sapphmap.hpp"
#include "external_memory/tools/kv_lsmtree.hpp"
#include "external_memory/tools/enigmap/enigmap_index_adapter.hpp"
#include "external_memory/tools/no_hashmap.hpp"
#include "external_memory/tools/h2o2ram/h2o2ram_adapter.hpp"

/**
 * @brief SGX OCALL-based file I/O helpers.
 *
 * Enclave code cannot call stdio APIs directly. All benchmark file accesses in
 * this header are funneled through the OCALL bridge exposed in Enclave_t.h.
 */
namespace ExpFileIO {

inline void open(const char* path) { ocall_exp_file_open(path); }

// Returns line length including '\n' when present (same as fgets+strlen), 0 = EOF
// before any payload, -1 = error.
inline int64_t readline(char* buf, uint64_t buf_size) {
    if (buf == nullptr || buf_size < 2U) return -1;
    int64_t ret = 0;
    const sgx_status_t st = ocall_exp_file_read_line(&ret, buf, buf_size);
    if (st != SGX_SUCCESS) return -1;
    return ret;
}

inline void close() { ocall_exp_file_close(); }

// Check existence: open, attempt one read, close.
// Returns false if file could not be opened (ret == -1 on first read).
inline bool exists(const char* path) {
    open(path);
    char buf[4];
    int64_t r = readline(buf, sizeof(buf));
    close();
    return (r >= 0);
}

} // namespace ExpFileIO

/**
 * @brief Lightweight parser utilities used by benchmark trace readers.
 *
 * These helpers intentionally avoid sscanf-style parsing to keep overhead low
 * in hot parsing loops.
 */
namespace ExpParse {

// Parse decimal uint64 from s; advance pointer past digits.
// Returns updated pointer, or nullptr on failure.
static inline const char* uint64(const char* s, uint64_t& out) {
    if (!s || *s < '0' || *s > '9') return nullptr;
    uint64_t val = 0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + static_cast<uint64_t>(*s - '0');
        ++s;
    }
    out = val;
    return s;
}

// Parse "YCSB_2^<digits>" from folder_name into power.
inline bool ycsb_power(std::string_view folder_name, unsigned int& power) {
    constexpr std::string_view kPrefix = "YCSB_2^";
    if (!folder_name.starts_with(kPrefix) || folder_name.size() <= kPrefix.size()) {
        return false;
    }

    const char* p = folder_name.data() + kPrefix.size();
    const char* const end = folder_name.data() + folder_name.size();
    if (*p < '0' || *p > '9') {
        return false;
    }

    unsigned int val = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        val = val * 10 + static_cast<unsigned int>(*p - '0');
        ++p;
    }

    if (p != end) {
        return false;
    }

    power = val;
    return true;
}

inline bool ycsb_power(const char* folder_name, unsigned int& power) {
    return folder_name != nullptr && ycsb_power(std::string_view(folder_name), power);
}

} // namespace ExpParse

/**
 * @brief Benchmark orchestration namespace for Exp1/Exp2 dispatch.
 */
namespace Experiment {

/** @brief Default YCSB dataset exponent used by the benchmark dispatcher. */
#ifndef BENCH_DATASET_POW
#define BENCH_DATASET_POW 24
#endif

/** @brief Top-level benchmark selector expected by run_bench_target(). */
#ifndef BENCH_TARGET_STR
#define BENCH_TARGET_STR "exp2_blocksize"
#endif

/** @brief Workload selector string for Exp1/workload dispatch. */
#ifndef BENCH_WORKLOAD_STR
#define BENCH_WORKLOAD_STR "ycsb_mix"
#endif

/** @brief Exp1/scalability folder override (e.g. YCSB_2^24). */
#ifndef BENCH_EXP1_FOLDER_STR
#define BENCH_EXP1_FOLDER_STR ""
#endif

/** @brief Build-time selector for NoLSMTree baseline index backend. */
#ifndef BENCH_USE_NOLSMTREE
#define BENCH_USE_NOLSMTREE 0
#endif

/** @brief Build-time selector for EnigMap index backend. */
#ifndef BENCH_USE_ENIGMAP
#define BENCH_USE_ENIGMAP 0
#endif

/** @brief Build-time selector for NoHashMap index backend. */
#ifndef BENCH_USE_NOHASHMAP
#define BENCH_USE_NOHASHMAP 0
#endif

#ifndef BENCH_USE_H2O2RAM
#define BENCH_USE_H2O2RAM 0
#endif

inline constexpr bool kCollectTradeoffTiming =
    std::string_view(BENCH_TARGET_STR) == "exp2_tradeoff";


// ============================================================================
// Configuration Parameters
// ============================================================================

/**
 * @brief Compile-time hardware capacity knobs passed from build flags.
 */
struct HardwareConfig {
    static constexpr uint64_t EnclaveSize   = ENCLAVE_SIZE;
    static constexpr uint64_t BufferSize    = BUFFER_SIZE;
    static constexpr uint64_t CachePageSize = BLOCK_SIZE;
};

/**
 * @brief Static structure parameters used by Exp1 core comparison entry points.
 */
struct StructureParams {

    inline static const uint64_t gamma = GAMMA;
    static constexpr double eps_total = EPS_TOTAL;

};

// --- Experiment Settings ---
// Base struct for experiment-specific workload parameters
struct ExpSettings {
    uint64_t init_size;       // Number of initial PUT operations
    uint64_t trace_size;      // Number of trace operations (queries/updates)
    uint64_t max_writes;      // Total operations: init_size + trace_size
    uint64_t domain_size;     // Key space size
    uint64_t point_query_ops; // Number of point queries (Exp1 only)
    uint64_t range_query_ops; // Number of range queries (Exp1 only)
};

// Exp1/scalability: YCSB size sweep - data folders YCSB_2^k; full sweep uses k in [17, 32]
// init_size = 2^k (from folder name)
// max_writes = init_size (bulk/seq build capacity; GET/SCAN use separate trace files)
// domain_size = 100 * init_size
inline constexpr uint64_t kExp1QueryTraceOpCount = 65536ULL;  // GET/SCAN ops per trace file

struct Exp1ScalabilitySettings : ExpSettings {
    explicit Exp1ScalabilitySettings(uint64_t data_size_power) {
        init_size = (1ULL << data_size_power);
        trace_size = 0;  // Exp1/scalability has separate query files
        point_query_ops = kExp1QueryTraceOpCount;
        range_query_ops = kExp1QueryTraceOpCount;
        max_writes = init_size;
        domain_size = 100ULL * max_writes;
    }
};

// Exp1/workload: YCSB replay workload
// init_size = 67108864, trace_size = 67108864
// max_writes = 2 * 67108864
// domain_size = 100 * (init_size + trace_size)
struct Exp1WorkloadYcsbSettings : ExpSettings {
    Exp1WorkloadYcsbSettings() {
        init_size = (1ULL << 26);
        trace_size = (1ULL << 26);
        point_query_ops = 0;
        range_query_ops = 0;
        max_writes = init_size + trace_size;
        domain_size = 100ULL * max_writes;
    }
};

// Exp1/workload: MetaKV trace (trimmed)
// init_size = 0, trace_size = 134217728
// max_writes = 134217728
// domain_size = UINT64_MAX
struct Exp1WorkloadMetakvSettings : ExpSettings {
    Exp1WorkloadMetakvSettings() {
        init_size = 0;
        trace_size = (1ULL << 27);
        point_query_ops = 0;
        range_query_ops = 0;
        max_writes = trace_size;
        domain_size = UINT64_MAX;
    }
};

// Exp1/workload: IBM ObjectStore trace (trimmed)
// init_size = 0, trace_size = 134217728
// max_writes = 134217728
// domain_size = UINT64_MAX
struct Exp1WorkloadIbmSettings : ExpSettings {
    Exp1WorkloadIbmSettings() {
        init_size = 0;
        trace_size = (1ULL << 27);
        point_query_ops = 0;
        range_query_ops = 0;
        max_writes = trace_size;
        domain_size = UINT64_MAX;
    }
};

// ============================================================================

enum class IndexType { Sapphmap, KVLsmtree, EnigMap, NoHashMap, H2O2RAM };
enum class ExpType {
    Exp1_Scalability,
    Exp1_Workload,
    Exp1_RangeQuery,
    Exp2_Parameters
};

// Batch size for file reading operations to reduce I/O context switches
static constexpr uint64_t kReadBatch = (1ULL << 20);  // 2^20

inline void fmtSize(uint64_t bytes, char* buf, size_t bufLen) {
    if (bytes >= 1024ULL * 1024 * 1024) {
        snprintf(buf, bufLen, "%lluGB",
                 static_cast<unsigned long long>(bytes / (1024ULL * 1024 * 1024)));
    } else if (bytes >= 1024ULL * 1024) {
        snprintf(buf, bufLen, "%lluMB",
                 static_cast<unsigned long long>(bytes / (1024ULL * 1024)));
    } else if (bytes >= 1024ULL) {
        snprintf(buf, bufLen, "%lluKB",
                 static_cast<unsigned long long>(bytes / 1024ULL));
    } else {
        snprintf(buf, bufLen, "%lluB",
                 static_cast<unsigned long long>(bytes));
    }
}

inline void print_backend_size_gb(uint64_t backend_bytes) {
    const double backend_gb = static_cast<double>(backend_bytes) /
                              (1024.0 * 1024.0 * 1024.0);
    printf("[Backend] size=%.2f GB (%llu bytes)\n",
           backend_gb,
           static_cast<unsigned long long>(backend_bytes));
}

inline void format_exp1_datasize_label(const std::string& folder_name,
                                       char* buf, size_t bufLen) {
    unsigned int p = 0;
    if (ExpParse::ycsb_power(folder_name.c_str(), p)) {
        snprintf(buf, bufLen, "2^%u", p);
        return;
    }
    snprintf(buf, bufLen, "%s", folder_name.c_str());
}

struct WorkloadFiles {
    enum class Mode : uint8_t {
        Exp1ScalabilitySplit,
        Exp1WorkloadYcsbReplay,
        Exp1WorkloadSingleTraceReplay
    };

    Mode mode = Mode::Exp1ScalabilitySplit;
    std::string init_path;
    std::string get_path;
    std::string trace_path;
};

inline bool file_exists(const std::string& path) {
    return ExpFileIO::exists(path.c_str());
}

// Exp1/scalability resolver: split init/get/scan files under benchmark/Exp1/scalability.
inline WorkloadFiles resolve_exp1_workload_files(const std::string& folder_path) {
    WorkloadFiles files;
    files.mode = WorkloadFiles::Mode::Exp1ScalabilitySplit;
    files.init_path = folder_path + "YCSBInitData.txt";
    files.get_path = folder_path + "YCSBGet.txt";
    return files;
}

// Exp1/workload resolver under benchmark/Exp1/workload/{ycsb|metakv|ibm}.
inline WorkloadFiles resolve_exp1_workload_replay_files(const std::string& folder_path,
                                                        const std::string& workload_name = "") {
    WorkloadFiles files;
    const std::string ycsb_init = folder_path + "YCSBInitData.txt";
    const std::string ycsb_mix = folder_path + "YCSBMixTrace.txt";
    const std::string ycsb_a_trace = folder_path + "YCSBATrace.txt";
    const bool force_mix =
        workload_name.find("mix") != std::string::npos;
    const bool force_ycsb_a =
        workload_name.find("ycsb_a") != std::string::npos ||
        workload_name.find("ycsba") != std::string::npos;

    if (force_mix && file_exists(ycsb_init) && file_exists(ycsb_mix)) {
        files.mode = WorkloadFiles::Mode::Exp1WorkloadYcsbReplay;
        files.init_path = ycsb_init;
        files.trace_path = ycsb_mix;
        return files;
    }
    if (force_ycsb_a && file_exists(ycsb_init) && file_exists(ycsb_a_trace)) {
        files.mode = WorkloadFiles::Mode::Exp1WorkloadYcsbReplay;
        files.init_path = ycsb_init;
        files.trace_path = ycsb_a_trace;
        return files;
    }

    if (file_exists(ycsb_init) && file_exists(ycsb_mix)) {
        files.mode = WorkloadFiles::Mode::Exp1WorkloadYcsbReplay;
        files.init_path = ycsb_init;
        files.trace_path = ycsb_mix;
        return files;
    }
    if (file_exists(ycsb_init) && file_exists(ycsb_a_trace)) {
        files.mode = WorkloadFiles::Mode::Exp1WorkloadYcsbReplay;
        files.init_path = ycsb_init;
        files.trace_path = ycsb_a_trace;
        return files;
    }

    const std::string metakv_trace = folder_path + "MetaKVTrace.txt";
    if (file_exists(metakv_trace)) {
        files.mode = WorkloadFiles::Mode::Exp1WorkloadSingleTraceReplay;
        files.init_path = "";
        files.trace_path = metakv_trace;
        return files;
    }

    const std::string ibm_trace = folder_path + "IBMObjectStoreTrace.txt";
    if (file_exists(ibm_trace)) {
        files.mode = WorkloadFiles::Mode::Exp1WorkloadSingleTraceReplay;
        files.init_path = "";
        files.trace_path = ibm_trace;
        return files;
    }

    // TODO: add explicit mappings for new Exp2 workload names.
    files.init_path = folder_path + "YCSBInitData.txt";
    files.trace_path = files.init_path;
    return files;
}

// Compatibility alias kept for downstream includes.
inline WorkloadFiles resolve_exp2_workload_files(const std::string& folder_path,
                                                 const std::string& workload_name = "") {
    return resolve_exp1_workload_replay_files(folder_path, workload_name);
}

enum class OpType : uint8_t { Invalid = 0, Put, Get, Delete, Scan };

struct ParsedOp {
    OpType type = OpType::Invalid;
    uint64_t key = 0;
    uint64_t value = 0;
    uint64_t right = 0;
};

[[noreturn]] inline void fail_fast(const char* phase,
                                   const std::string& path,
                                   const char* reason,
                                   const char* line) {
    printf("[FATAL] %s failed on %s: %s\n", phase, path.c_str(), reason);
    if (line) printf("  line: %s\n", line);
    ExpFileIO::close();
    std::abort();
}

// Manual parser: no sscanf needed.
inline bool parse_line(const char* line, ParsedOp& out) {
    const char* p = line;
    while (*p == ' ' || *p == '\t') ++p;

    if (p[0]=='P' && p[1]=='U' && p[2]=='T' && p[3]==' ') {
        p += 4;
        uint64_t k = 0, v = 0;
        p = ExpParse::uint64(p, k);
        if (!p || *p != ' ') return false;
        ++p;
        p = ExpParse::uint64(p, v);
        if (!p) return false;
        out.type = OpType::Put; out.key = k; out.value = v; out.right = 0;
        return true;
    }
    if (p[0]=='G' && p[1]=='E' && p[2]=='T' && p[3]==' ') {
        p += 4;
        uint64_t k = 0;
        p = ExpParse::uint64(p, k);
        if (!p) return false;
        out.type = OpType::Get; out.key = k; out.value = 0; out.right = 0;
        return true;
    }
    if (p[0]=='D' && p[1]=='E' && p[2]=='L' && p[3]=='E' &&
        p[4]=='T' && p[5]=='E' && p[6]==' ') {
        p += 7;
        uint64_t k = 0;
        p = ExpParse::uint64(p, k);
        if (!p) return false;
        out.type = OpType::Delete; out.key = k; out.value = 0; out.right = 0;
        return true;
    }
    if (p[0]=='S' && p[1]=='C' && p[2]=='A' && p[3]=='N' && p[4]==' ') {
        p += 5;
        uint64_t k = 0, len = 0;
        p = ExpParse::uint64(p, k);
        if (!p || *p != ' ') return false;
        ++p;
        p = ExpParse::uint64(p, len);
        if (!p) return false;
        out.type = OpType::Scan;
        out.key = k;
        out.right = (len == 0) ? k : (k + len - 1);
        out.value = out.right;
        return true;
    }
    return false;
}

// Batch load operations from file into a pre-allocated DatasetVec.
// Returns the actual number of operations loaded.
// File must already be opened via ExpFileIO::open().
// The batch_ops vector must be pre-allocated with the exact size to read.
template <typename DatasetVec>
inline uint64_t load_ops_batch(DatasetVec& batch_ops, 
                               const std::string& path,
                               const char* phase_name) {
    const uint64_t batch_size = batch_ops.size();
    typename DatasetVec::Writer writer(batch_ops.begin(), batch_ops.end());
    ParsedOp op;
    uint64_t count = 0;

    if (batch_size == 0) {
        writer.flush();
        return 0;
    }

    constexpr uint64_t kLineCap = 256;
    // Edger8r duplicates the ocall out-buffer inside the enclave (sgx_ocalloc);
    // peak ~= 2 * (lines * kLineCap). Split bulk reads so each ocall stays bounded.
    constexpr uint64_t kBulkReadMaxLines = (1ULL << 14);

    if (batch_size > (UINT64_MAX / kLineCap)) {
        fail_fast(phase_name, path, "batch too large for read buffer", nullptr);
    }

    while (count < batch_size) {
        const uint64_t want = std::min(kBulkReadMaxLines, batch_size - count);
        const uint64_t chunk_cap = want * kLineCap;
        std::vector<char> scratch(chunk_cap + 1);

        int64_t nbytes = -1;
        const sgx_status_t st = ocall_exp_file_read_bulk(
            &nbytes, want, chunk_cap, scratch.data());

        if (st != SGX_SUCCESS) {
            printf("[ERROR] ocall read bulk failed on %s\n", path.c_str());
            writer.flush();
            return count;
        }
        if (nbytes < 0) {
            printf("[ERROR] Cannot read file: %s\n", path.c_str());
            writer.flush();
            return count;
        }

        scratch[static_cast<size_t>(nbytes)] = '\0';

        uint64_t line_begin = 0;
        uint64_t got_chunk = 0;
        while (got_chunk < want &&
               line_begin < static_cast<uint64_t>(nbytes)) {
            uint64_t j = line_begin;
            while (j < static_cast<uint64_t>(nbytes) && scratch[j] != '\n') {
                ++j;
            }
            if (j >= static_cast<uint64_t>(nbytes)) {
                break;
            }
            scratch[j] = '\0';
            if (!parse_line(&scratch[line_begin], op)) {
                fail_fast(phase_name, path, "parse_line failed", &scratch[line_begin]);
            }
            writer.write(op);
            ++count;
            ++got_chunk;
            line_begin = j + 1;
        }

        if (got_chunk != want) {
            break;
        }
    }
    writer.flush();

    if (count != batch_size) {
        printf("[WARN] %s: Expected to read %llu ops but got %llu\n",
               phase_name,
               static_cast<unsigned long long>(batch_size),
               static_cast<unsigned long long>(count));
    }

    return count;
}

// Bulk-read PUT lines into a contiguous [seg_begin, seg_end) segment of NCVec<KvPair>.
// File must already be open; segment size must equal the number of lines to read.
template <typename DatasetVec>
inline uint64_t load_put_kv_segment(typename DatasetVec::Iterator seg_begin,
                                    typename DatasetVec::Iterator seg_end,
                                    const std::string& path,
                                    const char* phase_name) {
    using KvPair = EM::Sapphmap::KvPair;

    const uint64_t batch_size = static_cast<uint64_t>(seg_end - seg_begin);
    typename DatasetVec::Writer writer(seg_begin, seg_end);
    ParsedOp op;
    uint64_t count = 0;

    if (batch_size == 0) {
        writer.flush();
        return 0;
    }

    constexpr uint64_t kLineCap = 256;
    constexpr uint64_t kBulkReadMaxLines = (1ULL << 14);

    if (batch_size > (UINT64_MAX / kLineCap)) {
        fail_fast(phase_name, path, "batch too large for read buffer", nullptr);
    }

    while (count < batch_size) {
        const uint64_t want = std::min(kBulkReadMaxLines, batch_size - count);
        const uint64_t chunk_cap = want * kLineCap;
        std::vector<char> scratch(chunk_cap + 1);

        int64_t nbytes = -1;
        const sgx_status_t st = ocall_exp_file_read_bulk(
            &nbytes, want, chunk_cap, scratch.data());

        if (st != SGX_SUCCESS) {
            printf("[ERROR] ocall read bulk failed on %s\n", path.c_str());
            writer.flush();
            return count;
        }
        if (nbytes < 0) {
            printf("[ERROR] Cannot read file: %s\n", path.c_str());
            writer.flush();
            return count;
        }

        scratch[static_cast<size_t>(nbytes)] = '\0';

        uint64_t line_begin = 0;
        uint64_t got_chunk = 0;
        while (got_chunk < want &&
               line_begin < static_cast<uint64_t>(nbytes)) {
            uint64_t j = line_begin;
            while (j < static_cast<uint64_t>(nbytes) && scratch[j] != '\n') {
                ++j;
            }
            if (j >= static_cast<uint64_t>(nbytes)) {
                break;
            }
            scratch[j] = '\0';
            if (!parse_line(&scratch[line_begin], op)) {
                fail_fast(phase_name, path, "parse_line failed", &scratch[line_begin]);
            }
            if (op.type != OpType::Put) {
                fail_fast(phase_name, path, "expected only PUT in init dataset",
                          &scratch[line_begin]);
            }
            KvPair kv;
            kv.key = op.key;
            kv.value = op.value;
            writer.write(kv);
            ++count;
            ++got_chunk;
            line_begin = j + 1;
        }

        if (got_chunk != want) {
            break;
        }
    }
    writer.flush();

    if (count != batch_size) {
        printf("[WARN] %s: Expected to read %llu KVs but got %llu\n",
               phase_name,
               static_cast<unsigned long long>(batch_size),
               static_cast<unsigned long long>(count));
    }

    return count;
}

inline void print_batch_progress(const char* phase_name,
                                 uint64_t processed_ops,
                                 uint64_t total_ops) {
    const uint64_t safe_total = (total_ops == 0) ? 1ULL : total_ops;
    const uint64_t safe_processed =
        (processed_ops > total_ops) ? total_ops : processed_ops;
    const double pct = (static_cast<double>(safe_processed) * 100.0) /
                       static_cast<double>(safe_total);
    printf("[Progress] %s: %.2f%% (%llu/%llu)\n",
           phase_name,
           pct,
           static_cast<unsigned long long>(safe_processed),
           static_cast<unsigned long long>(total_ops));
}

inline uint64_t parse_expected_from_datasize_label(std::string_view data_size_label) {
    // Expected format: "2^k"
    if (data_size_label.size() < 3) return 0;
    if (data_size_label[0] != '2' || data_size_label[1] != '^') return 0;
    uint64_t p = 0;
    for (size_t i = 2; i < data_size_label.size(); ++i) {
        const char c = data_size_label[i];
        if (c < '0' || c > '9') return 0;
        p = p * 10 + static_cast<uint64_t>(c - '0');
    }
    if (p >= 63ULL) return 0;
    return (1ULL << p);
}

template <typename IndexT>
struct IndexRunner;

template <uint64_t CPS, uint64_t ES>
struct IndexRunner<EM::Sapphmap::SapphMap<CPS, ES>> {
    using Index = EM::Sapphmap::SapphMap<CPS, ES>;

    static Index create(uint64_t max_writes, bool static_mode) {
        const double delta_total = 1.0 / (2.0 * std::pow(static_cast<double>(max_writes), 1.2));
        return Index(HardwareConfig::BufferSize, StructureParams::gamma, max_writes,
                     StructureParams::eps_total, delta_total, static_mode);
    }
};

template <uint64_t CPS, uint64_t ES>
struct IndexRunner<EM::Sapphmap::KVLSMTree<CPS, ES>> {
    using Index = EM::Sapphmap::KVLSMTree<CPS, ES>;

    static Index create(uint64_t max_writes, bool /*static_mode*/) {
        return Index(HardwareConfig::BufferSize, StructureParams::gamma, max_writes);
    }
};

template <uint64_t CPS, uint64_t ES>
struct IndexRunner<EnigMapIndexAdapter<CPS, ES>> {
    using Index = EnigMapIndexAdapter<CPS, ES>;

    static Index create(uint64_t max_writes, bool /*static_mode*/) {
        return Index(max_writes);
    }
};

template <uint64_t CPS, uint64_t ES>
struct IndexRunner<EM::Sapphmap::NoHashMap<CPS, ES>> {
    using Index = EM::Sapphmap::NoHashMap<CPS, ES>;

    static Index create(uint64_t max_writes, bool /*static_mode*/) {
        return Index(HardwareConfig::BufferSize, StructureParams::gamma, max_writes);
    }
};

template <uint64_t CPS, uint64_t ES>
struct IndexRunner<H2O2RAMAdapter<CPS, ES>> {
    using Index = H2O2RAMAdapter<CPS, ES>;
    static Index create(uint64_t max_writes, bool) { return Index(max_writes); }
};

template <typename IndexT>
struct PaddedSegInfoGetter;

template <typename IndexT>
struct IsEnigMapIndex : std::false_type {};

template <uint64_t CPS, uint64_t ES>
struct IsEnigMapIndex<EnigMapIndexAdapter<CPS, ES>> : std::true_type {};

template <typename IndexT>
inline constexpr bool kIsEnigMapIndex = IsEnigMapIndex<IndexT>::value;

template <typename IndexT>
struct IsH2O2RAMIndex : std::false_type {};

template <uint64_t CPS, uint64_t ES>
struct IsH2O2RAMIndex<H2O2RAMAdapter<CPS, ES>> : std::true_type {};

template <typename IndexT>
inline constexpr bool kIsH2O2RAMIndex = IsH2O2RAMIndex<IndexT>::value;

template <typename IndexT>
struct IsNoHashMapIndex : std::false_type {};

template <uint64_t CPS, uint64_t ES>
struct IsNoHashMapIndex<EM::Sapphmap::NoHashMap<CPS, ES>>
    : std::true_type {};

template <typename IndexT>
inline constexpr bool kIsNoHashMapIndex = IsNoHashMapIndex<IndexT>::value;

template <typename IndexT>
inline uint64_t exp1_tree_capacity_writes(const ExpSettings& exp_settings) {
    if constexpr (kIsEnigMapIndex<IndexT>) {
        if (exp_settings.init_size > (UINT64_MAX - 1024ULL) / 2ULL) {
            return UINT64_MAX;
        }
        const uint64_t seq_n = exp_settings.init_size * 2ULL + 1024ULL;
        return (seq_n == 0) ? 1ULL : seq_n;
    } else {
        return (exp_settings.max_writes == 0) ? 1ULL : exp_settings.max_writes;
    }
}

template <typename IndexT>
inline uint64_t exp1_reported_size_bytes(const IndexT& index,
                                         const ExpSettings& exp_settings) {
    if constexpr (kIsEnigMapIndex<IndexT>) {
        return IndexT::ComputeReportedTreeBytesByWrites(exp_settings.init_size);
    } else {
        (void)exp_settings;
        return index.getByteSize();
    }
}

template <typename IndexT>
inline double bplus_tree_size_mb(const IndexT& index) {
    if constexpr (requires { index.getBPlusTreeByteSize(); }) {
        return static_cast<double>(index.getBPlusTreeByteSize()) /
               (1024.0 * 1024.0);
    } else {
        return 0.0;
    }
}

template <typename IndexT>
inline uint64_t exp1_workload_tree_capacity_writes(const ExpSettings& exp_settings) {
    if constexpr (kIsEnigMapIndex<IndexT>) {
        if (exp_settings.max_writes > (UINT64_MAX - 1024ULL) / 2ULL) {
            return UINT64_MAX;
        }
        const uint64_t seq_n = exp_settings.max_writes * 2ULL + 1024ULL;
        return (seq_n == 0) ? 1ULL : seq_n;
    } else {
        return (exp_settings.max_writes == 0) ? 1ULL : exp_settings.max_writes;
    }
}

template <typename IndexT>
inline constexpr bool kExp1WorkloadBatchProgress = kIsEnigMapIndex<IndexT>;

template <typename IndexT, typename = void>
struct Exp1BulkBuildSupport {
    static constexpr bool value = true;
};

template <typename IndexT>
struct Exp1BulkBuildSupport<IndexT, std::void_t<decltype(IndexT::kSupportsExp1BulkBuild)>> {
    static constexpr bool value = IndexT::kSupportsExp1BulkBuild;
};

template <typename IndexT>
struct PaddedSegInfo {
    uint64_t z_out = 0;
    uint64_t z_raw = 0;
    /// Non-zero for KVLSMTree: print items_per_page instead of Z_out / Z_raw.
    uint64_t lsm_items_per_page = 0;
};

inline void print_buffer_and_segment_line(uint64_t buffer_items,
                                          uint64_t z_out,
                                          uint64_t z_raw,
                                          uint64_t lsm_items_per_page) {
    if (lsm_items_per_page != 0) {
        printf("  Buffer: %llu items; items_per_page = %llu\n",
               static_cast<unsigned long long>(buffer_items),
               static_cast<unsigned long long>(lsm_items_per_page));
    } else {
        printf("  Buffer: %llu items; padded segment size (Z_out) = %llu items; "
               "raw segment size (Z_raw) = %llu items\n",
               static_cast<unsigned long long>(buffer_items),
               static_cast<unsigned long long>(z_out),
               static_cast<unsigned long long>(z_raw));
    }
}

template <uint64_t CPS, uint64_t ES>
struct PaddedSegInfoGetter<EM::Sapphmap::SapphMap<CPS, ES>> {
    static PaddedSegInfo<EM::Sapphmap::SapphMap<CPS, ES>>
    get(const EM::Sapphmap::SapphMap<CPS, ES>& index) {
        PaddedSegInfo<EM::Sapphmap::SapphMap<CPS, ES>> info{};
        if (index.depth > 0) {
            const auto& ls = index.levels[index.depth];
            if (ls.is_enclave && ls.enc_pds) {
                info.z_out = ls.enc_pds->Z;
                info.z_raw = ls.enc_pds->Z_raw;
            } else if (ls.ext_pds) {
                info.z_out = ls.ext_pds->Z;
                info.z_raw = ls.ext_pds->Z_raw;
            }
        }
        return info;
    }
};

template <uint64_t CPS, uint64_t ES>
struct PaddedSegInfoGetter<EM::Sapphmap::KVLSMTree<CPS, ES>> {
    static PaddedSegInfo<EM::Sapphmap::KVLSMTree<CPS, ES>>
    get(const EM::Sapphmap::KVLSMTree<CPS, ES>& /*index*/) {
        PaddedSegInfo<EM::Sapphmap::KVLSMTree<CPS, ES>> info{};
        info.lsm_items_per_page = EM::Sapphmap::KVLSMTree<CPS, ES>::kItemsPerPage;
        return info;
    }
};

template <uint64_t CPS, uint64_t ES>
struct PaddedSegInfoGetter<EnigMapIndexAdapter<CPS, ES>> {
    static PaddedSegInfo<EnigMapIndexAdapter<CPS, ES>>
    get(const EnigMapIndexAdapter<CPS, ES>& /*index*/) {
        return PaddedSegInfo<EnigMapIndexAdapter<CPS, ES>>{};
    }
};

template <uint64_t CPS, uint64_t ES>
struct PaddedSegInfoGetter<EM::Sapphmap::NoHashMap<CPS, ES>> {
    static PaddedSegInfo<EM::Sapphmap::NoHashMap<CPS, ES>>
    get(const EM::Sapphmap::NoHashMap<CPS, ES>& /*index*/) {
        return PaddedSegInfo<EM::Sapphmap::NoHashMap<CPS, ES>>{};
    }
};

template <uint64_t CPS, uint64_t ES>
struct PaddedSegInfoGetter<H2O2RAMAdapter<CPS, ES>> {
    static PaddedSegInfo<H2O2RAMAdapter<CPS, ES>>
    get(const H2O2RAMAdapter<CPS, ES>&) {
        return PaddedSegInfo<H2O2RAMAdapter<CPS, ES>>{};
    }
};

template <typename IndexT>
inline uint64_t compute_backend_size_bytes(const ExpSettings& exp_settings,
                                           WorkloadFiles::Mode mode) {
    if constexpr (kIsEnigMapIndex<IndexT>) {
        if (mode == WorkloadFiles::Mode::Exp1ScalabilitySplit) {
            return IndexT::RecommendBackendSize(
                exp_settings.init_size,
                exp_settings.max_writes,
                true);
        }

        const uint64_t workload_writes = exp1_workload_tree_capacity_writes<IndexT>(exp_settings);
        if (workload_writes == UINT64_MAX) {
            return UINT64_MAX;
        }
        const uint64_t backend = IndexT::ComputeBackendBytes(workload_writes);
        return backend + backend / 2;
    } else if constexpr (kIsH2O2RAMIndex<IndexT>) {
        if (mode == WorkloadFiles::Mode::Exp1ScalabilitySplit) {
            return IndexT::RecommendBackendSize(
                exp_settings.init_size,
                exp_settings.max_writes,
                true);
        }

        const uint64_t workload_writes = exp1_workload_tree_capacity_writes<IndexT>(exp_settings);
        if (workload_writes == UINT64_MAX) {
            return UINT64_MAX;
        }
        const uint64_t backend = IndexT::ComputeBackendBytes(workload_writes);
        return backend + backend / 2;
    } else if constexpr (kIsNoHashMapIndex<IndexT>) {
        return IndexT::RecommendBackendSize(
            exp_settings.init_size,
            exp_settings.max_writes,
            mode == WorkloadFiles::Mode::Exp1ScalabilitySplit);
    } else {
        using Item = EM::Definition::Item;
        const uint64_t backend_items =
            ((exp_settings.init_size > exp_settings.max_writes)
                 ? exp_settings.init_size
                 : exp_settings.max_writes) +
            1;
        return 8 * sizeof(Item) * backend_items +
               8 * sizeof(uint64_t) * backend_items;
    }
}

struct TimedReplayStats {
    uint64_t processed_ops = 0;
    uint64_t elapsed_ns = 0;
};

struct WorkloadTimeBreakdown {
    uint64_t write_cycles = 0;
    uint64_t query_cycles = 0;
    uint64_t write_ops = 0;
    uint64_t query_ops = 0;
};

struct WorkloadElapsedBreakdown {
    uint64_t write_ns = 0;
    uint64_t query_ns = 0;
};

inline uint64_t read_workload_tsc_begin() {
    uint32_t lo = 0;
    uint32_t hi = 0;
    __asm__ __volatile__(
        "lfence\n\t"
        "rdtsc"
        : "=a"(lo), "=d"(hi)
        :
        : "memory");
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

inline uint64_t read_workload_tsc_end() {
    uint32_t lo = 0;
    uint32_t hi = 0;
    uint32_t aux = 0;
    __asm__ __volatile__(
        "rdtscp\n\t"
        "lfence"
        : "=a"(lo), "=d"(hi), "=c"(aux)
        :
        : "memory");
    (void)aux;
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

inline WorkloadElapsedBreakdown split_workload_elapsed(
    uint64_t trace_elapsed_ns,
    const WorkloadTimeBreakdown& cycles) {
    WorkloadElapsedBreakdown elapsed;
    const uint64_t total_cycles = cycles.write_cycles + cycles.query_cycles;
    if (total_cycles == 0) {
        elapsed.query_ns = trace_elapsed_ns;
        return elapsed;
    }

    const long double write_fraction =
        static_cast<long double>(cycles.write_cycles) /
        static_cast<long double>(total_cycles);
    elapsed.write_ns = static_cast<uint64_t>(
        static_cast<long double>(trace_elapsed_ns) * write_fraction + 0.5L);
    if (elapsed.write_ns > trace_elapsed_ns) {
        elapsed.write_ns = trace_elapsed_ns;
    }
    elapsed.query_ns = trace_elapsed_ns - elapsed.write_ns;
    return elapsed;
}

struct NoBatchProgress {
    void operator()(uint64_t /*processed*/, uint64_t /*total*/) const {}
};

/**
 * @brief Replay a trace file in fixed-size batches and time only index operations.
 */
template <typename OpVec, typename OnOp, typename OnBatchProgress = NoBatchProgress>
inline TimedReplayStats replay_ops_batched(const std::string& path,
                                           uint64_t total_ops,
                                           uint64_t batch_ops,
                                           const char* phase_name,
                                           OnOp&& on_op,
                                           OnBatchProgress&& on_batch_progress = {}) {
    TimedReplayStats stats;
    if (total_ops == 0) {
        ExpFileIO::open(path.c_str());
        ExpFileIO::close();
        return stats;
    }
    if (batch_ops == 0) {
        fail_fast(phase_name, path, "batch size must be > 0", nullptr);
    }

    ExpFileIO::open(path.c_str());

    const uint64_t num_full_batches = total_ops / batch_ops;
    const uint64_t remaining_ops = total_ops % batch_ops;

    auto run_one_batch = [&](uint64_t expected) {
        OpVec batch(expected);
        const uint64_t got = load_ops_batch(batch, path, phase_name);
        if (got != expected) {
            fail_fast(phase_name, path, "unexpected batch size", nullptr);
        }

        uint64_t t0 = 0;
        uint64_t t1 = 0;
        ocall_measure_time(&t0);

        typename OpVec::Reader reader(batch.begin(), batch.end());
        for (uint64_t i = 0; i < got; ++i) {
            on_op(reader.read());
        }

        ocall_measure_time(&t1);
        stats.elapsed_ns += (t1 - t0);
        stats.processed_ops += got;
        on_batch_progress(stats.processed_ops, total_ops);
    };

    for (uint64_t batch_idx = 0; batch_idx < num_full_batches; ++batch_idx) {
        (void)batch_idx;
        run_one_batch(batch_ops);
    }
    if (remaining_ops > 0) {
        run_one_batch(remaining_ops);
    }

    ExpFileIO::close();
    return stats;
}

template <typename OpVec, typename OnOp>
inline TimedReplayStats replay_ops_single_batch(const std::string& path,
                                                uint64_t total_ops,
                                                const char* phase_name,
                                                OnOp&& on_op) {
    return replay_ops_batched<OpVec>(
        path,
        total_ops,
        (total_ops == 0) ? 1ULL : total_ops,
        phase_name,
        std::forward<OnOp>(on_op));
}

template <typename DatasetVec>
inline void load_put_dataset_for_ingest(DatasetVec& dataset,
                                        const std::string& path,
                                        const char* phase_name) {
    const uint64_t total_ops = dataset.size();
    if (total_ops == 0) {
        return;
    }

    const uint64_t batch_ops = std::min(kReadBatch, total_ops);
    if (batch_ops == 0) {
        fail_fast(phase_name, path, "batch size must be > 0", nullptr);
    }

    ExpFileIO::open(path.c_str());

    const uint64_t num_full_batches = total_ops / batch_ops;
    const uint64_t remaining_ops = total_ops % batch_ops;
    uint64_t offset = 0;

    auto load_one_segment = [&](uint64_t count) {
        const auto seg_begin = dataset.begin() + offset;
        const auto seg_end = dataset.begin() + offset + count;
        const uint64_t got =
            load_put_kv_segment<DatasetVec>(seg_begin, seg_end, path, phase_name);
        if (got != count) {
            fail_fast(phase_name, path, "unexpected batch size", nullptr);
        }
        offset += count;
    };

    for (uint64_t batch_idx = 0; batch_idx < num_full_batches; ++batch_idx) {
        (void)batch_idx;
        load_one_segment(batch_ops);
    }
    if (remaining_ops > 0) {
        load_one_segment(remaining_ops);
    }

    ExpFileIO::close();
}

template <typename IndexT>
inline void apply_replay_trace_op(IndexT& index,
                                  const ParsedOp& op,
                                  const std::string& path,
                                  const char* phase_name) {
    switch (op.type) {
        case OpType::Put:
            index.Insert(op.key, op.value);
            return;
        case OpType::Get:
            index.PointQuery(op.key);
            return;
        case OpType::Delete:
            index.Delete(op.key);
            return;
        case OpType::Scan:
            index.RangeQuery(op.key, op.right);
            return;
        default:
            fail_fast(phase_name, path, "unexpected op type", nullptr);
    }
}

template <typename IndexT>
inline void apply_replay_trace_op_timed(
    IndexT& index,
    const ParsedOp& op,
    const std::string& path,
    const char* phase_name,
    WorkloadTimeBreakdown& breakdown) {
    const bool is_write = op.type == OpType::Put || op.type == OpType::Delete;
    const bool is_query = op.type == OpType::Get || op.type == OpType::Scan;
    if (!is_write && !is_query) {
        fail_fast(phase_name, path, "unexpected op type", nullptr);
    }

    const uint64_t t0 = read_workload_tsc_begin();
    apply_replay_trace_op(index, op, path, phase_name);
    const uint64_t elapsed_cycles = read_workload_tsc_end() - t0;

    if (is_write) {
        breakdown.write_cycles += elapsed_cycles;
        ++breakdown.write_ops;
    } else {
        breakdown.query_cycles += elapsed_cycles;
        ++breakdown.query_ops;
    }
}

template <typename IndexT>
inline void apply_replay_trace_op_for_target(
    IndexT& index,
    const ParsedOp& op,
    const std::string& path,
    const char* phase_name,
    WorkloadTimeBreakdown& breakdown) {
    if constexpr (kCollectTradeoffTiming) {
        apply_replay_trace_op_timed(index, op, path, phase_name, breakdown);
    } else {
        apply_replay_trace_op(index, op, path, phase_name);
    }
}

template <typename IndexT>
inline void insert_new(IndexT& index, uint64_t key, uint64_t value) {
    if constexpr (requires { index.InsertNew(key, value); }) {
        index.InsertNew(key, value);
    } else {
        index.Insert(key, value);
    }
}

template <typename IndexT>
inline uint64_t finalize_h2o2ram_phase(IndexT& index) {
    if constexpr (kIsH2O2RAMIndex<IndexT>) {
        uint64_t t0 = 0;
        uint64_t t1 = 0;
        ocall_measure_time(&t0);
        index.Flush();
        ocall_measure_time(&t1);
        return t1 - t0;
    }
    return 0;
}

template <typename IndexT>
/**
 * @brief Execute one Exp1 sub-run for a concrete index type.
 *
 * The function preserves the original benchmark phases and timer boundaries:
 * bulk-build (optional), sequential build, and replay/query phases.
 */
void run_index_test_impl(const std::string& folder_path,
                         const WorkloadFiles& files,
                         const std::string& data_size_label,
                         const char* index_name,
                         bool support_static_mode,
                         const ExpSettings& exp_settings) {
    using KvPair = EM::Sapphmap::KvPair;
    using DatasetVec = typename IndexT::template NCVec<KvPair>;

    const std::string init_path = files.init_path;
    const std::string trace_path = files.trace_path;

    EM::Definition::DOMAIN_SIZE_D = exp_settings.domain_size;

    char esBuf[32], bsBuf[32], cpsBuf[32];
    fmtSize(HardwareConfig::EnclaveSize, esBuf, sizeof(esBuf));
    fmtSize(HardwareConfig::BufferSize, bsBuf, sizeof(bsBuf));
    fmtSize(HardwareConfig::CachePageSize, cpsBuf, sizeof(cpsBuf));

    if (files.mode == WorkloadFiles::Mode::Exp1ScalabilitySplit) {
        printf("========== Exp1/scalability: Core Comparison (Single Run) ==========\n");
        printf("[Exp1/scalability] structure=%s eps=%.4f gamma=%llu buffer=%s block=%s enclave=%s data=%s "
               "init_ops=%llu get_ops=%llu scan_ops=%llu (range phase off) domain=%llu\n",
               index_name,
               StructureParams::eps_total,
               static_cast<unsigned long long>(StructureParams::gamma),
               bsBuf, cpsBuf, esBuf, data_size_label.c_str(),
               static_cast<unsigned long long>(exp_settings.init_size),
               static_cast<unsigned long long>(exp_settings.point_query_ops),
               static_cast<unsigned long long>(0),
               static_cast<unsigned long long>(EM::Definition::DOMAIN_SIZE_D));
        printf("folder=%s\n", folder_path.c_str());
    } else {
        printf("========== Exp1/workload: Core Comparison (Single Run) ==========\n");
        printf("[Exp1/workload] structure=%s eps=%.4f gamma=%llu buffer=%s block=%s enclave=%s workload=%s "
               "init_ops=%llu trace_ops=%llu domain=%llu\n",
               index_name,
               StructureParams::eps_total,
               static_cast<unsigned long long>(StructureParams::gamma),
               bsBuf, cpsBuf, esBuf, data_size_label.c_str(),
               static_cast<unsigned long long>(exp_settings.init_size),
               static_cast<unsigned long long>(exp_settings.trace_size),
               static_cast<unsigned long long>(EM::Definition::DOMAIN_SIZE_D));
        printf("folder=%s\n", folder_path.c_str());
        if (!files.init_path.empty()) {
            printf("init_file=%s\n", files.init_path.c_str());
        }
        if (!files.trace_path.empty()) {
            printf("trace_file=%s\n", files.trace_path.c_str());
        }
    }

    const uint64_t backend_bytes =
        compute_backend_size_bytes<IndexT>(exp_settings, files.mode);
    if (backend_bytes == UINT64_MAX) {
        fail_fast("backend_size", folder_path,
                  "backend size overflow", nullptr);
    }
    if (EM::Backend::g_DefaultBackend) delete EM::Backend::g_DefaultBackend;
    EM::Backend::g_DefaultBackend =
        new EM::Backend::MemServerBackend(backend_bytes);
    if constexpr (kIsH2O2RAMIndex<IndexT> || kIsNoHashMapIndex<IndexT>) {
        print_backend_size_gb(backend_bytes);
    }

    if (files.mode == WorkloadFiles::Mode::Exp1ScalabilitySplit) {
        double bulk_build_s = 0.0;
        uint64_t bulk_insert_cnt = 0;
        constexpr bool enable_exp1_bulk_build = Exp1BulkBuildSupport<IndexT>::value;
        const uint64_t exp1_tree_writes = exp1_tree_capacity_writes<IndexT>(exp_settings);

        if (exp1_tree_writes == UINT64_MAX) {
            fail_fast("exp1_capacity", folder_path,
                      "exp1 tree capacity overflow", nullptr);
        }

        // --- Phase 1: Bulk Build ---
        if constexpr (enable_exp1_bulk_build) {
            IndexT index = IndexRunner<IndexT>::create(exp1_tree_writes,
                                                       support_static_mode);
            DatasetVec dataset(exp_settings.init_size);
            if (exp_settings.init_size > 0) {
                load_put_dataset_for_ingest(dataset, init_path, "exp1_bulk_ingest_load");
            }
            bulk_insert_cnt = exp_settings.init_size;  // count outside timing

            uint64_t t0 = 0, t1 = 0;
            ocall_measure_time(&t0);
            index.Ingest(dataset);
            ocall_measure_time(&t1);
            bulk_build_s = static_cast<double>(t1 - t0) / 1e9;
        }

        double seq_build_s = 0.0;
        double seq_size_mb = 0.0;
        double seq_bptree_size_mb = 0.0;
        double point_avg_us = 0.0;
        uint64_t seq_insert_cnt = 0;
        uint64_t seq_depth = 0, seq_enclave_levels = 0, seq_buffer_items = 0;
        uint64_t seq_padded_seg_size = 0, seq_raw_seg_size = 0;
        uint64_t seq_lsm_items_per_page = 0;

        // --- Phase 2: Seq Build & Queries ---
        {
            IndexT index = IndexRunner<IndexT>::create(exp1_tree_writes, false);

            // Sub-Phase: Seq Build
            {
                using OpVec = typename IndexT::template NCVec<ParsedOp>;

                TimedReplayStats seq_build_stats = replay_ops_batched<OpVec>(
                    init_path,
                    exp_settings.init_size,
                    kReadBatch,
                    "exp1_seq_build",
                    [&](const ParsedOp& op) {
                        if (op.type != OpType::Put) {
                            fail_fast("exp1_seq_build", init_path,
                                      "unexpected op, expected PUT", nullptr);
                        }
                        insert_new(index, op.key, op.value);
                        ++seq_insert_cnt;
                    });
                seq_build_stats.elapsed_ns += finalize_h2o2ram_phase(index);
                seq_build_s = static_cast<double>(seq_build_stats.elapsed_ns) / 1e9;
            }

            // Sub-Phase: Point Query
            {
                using OpVec = typename IndexT::template NCVec<ParsedOp>;
                const std::string& get_path = files.get_path;

                TimedReplayStats point_query_stats = replay_ops_single_batch<OpVec>(
                    get_path,
                    exp_settings.point_query_ops,
                    "exp1_point_query",
                    [&](const ParsedOp& op) {
                        if (op.type != OpType::Get) {
                            fail_fast("exp1_point_query", get_path,
                                      "unexpected op, expected GET", nullptr);
                        }
                        index.PointQuery(op.key);
                    });
                point_query_stats.elapsed_ns += finalize_h2o2ram_phase(index);

                point_avg_us = (point_query_stats.processed_ops == 0)
                    ? 0.0
                    : static_cast<double>(point_query_stats.elapsed_ns) / 1e3 /
                          static_cast<double>(point_query_stats.processed_ops);
            }

            seq_depth = index.level_count();
            seq_enclave_levels = index.enclave_levels;
            seq_buffer_items = index.buffer_real_count;
            const auto seg_info = PaddedSegInfoGetter<IndexT>::get(index);
            seq_padded_seg_size = seg_info.z_out;
            seq_raw_seg_size = seg_info.z_raw;
            seq_lsm_items_per_page = seg_info.lsm_items_per_page;
            const uint64_t seq_size_bytes =
                exp1_reported_size_bytes(index, exp_settings);
            seq_size_mb = static_cast<double>(seq_size_bytes) / (1024.0 * 1024.0);
            seq_bptree_size_mb = bplus_tree_size_mb(index);
        }

        printf("  Levels: %llu total, %llu in enclave\n",
               static_cast<unsigned long long>(seq_depth),
               static_cast<unsigned long long>(seq_enclave_levels));
        print_buffer_and_segment_line(seq_buffer_items, seq_padded_seg_size, seq_raw_seg_size,
                                      seq_lsm_items_per_page);
        const uint64_t expected_by_label = parse_expected_from_datasize_label(data_size_label);
        if (expected_by_label == 0) {
            fail_fast("exp1_size_check", folder_path, "DataSize label is not parseable as 2^k", data_size_label.c_str());
        }
        if constexpr (enable_exp1_bulk_build) {
            if (bulk_insert_cnt != expected_by_label ||
                seq_insert_cnt != expected_by_label ||
                bulk_insert_cnt != seq_insert_cnt) {
                fail_fast("exp1_size_check", folder_path, "bulk/seq/datasize counts mismatch", nullptr);
            }
        } else {
            if (seq_insert_cnt != expected_by_label) {
                fail_fast("exp1_size_check", folder_path, "seq/datasize counts mismatch", nullptr);
            }
        }
        {
            if constexpr (enable_exp1_bulk_build) {
                printf("[Exp1/scalability] bulk_load=%.4fs seq_build=%.4fs pq=%.4f us/op size=%.4f MB",
                       bulk_build_s, seq_build_s, point_avg_us, seq_size_mb);
            } else {
                printf("[Exp1/scalability] bulk_load=skipped seq_build=%.4fs pq=%.4f us/op size=%.4f MB",
                       seq_build_s, point_avg_us, seq_size_mb);
            }
            if constexpr (requires(const IndexT& idx) { idx.getBPlusTreeByteSize(); }) {
                printf(" bptree_size=%.4f MB", seq_bptree_size_mb);
            }
            printf(" (rq skipped)\n");
        }
        // Both index objects were already in inner scopes above and are already
        // destroyed at this point.  Clean up the backend before returning.
        delete EM::Backend::g_DefaultBackend;
        EM::Backend::g_DefaultBackend = nullptr;
        return;

    } else if (files.mode == WorkloadFiles::Mode::Exp1WorkloadYcsbReplay) {
        using OpVec = typename IndexT::template NCVec<ParsedOp>;
        const uint64_t workload_tree_writes =
            exp1_workload_tree_capacity_writes<IndexT>(exp_settings);
        if (workload_tree_writes == UINT64_MAX) {
            fail_fast("exp1_workload_capacity", folder_path,
                      "tree capacity overflow", nullptr);
        }
        constexpr bool enable_batch_progress = kExp1WorkloadBatchProgress<IndexT>;

        // Hoist result variables outside the index scope so they survive
        // the index destructor and can be printed after cleanup.
        double   init_s = 0.0, trace_s = 0.0, size_mb = 0.0;
        double   bptree_size_mb = 0.0;
        WorkloadTimeBreakdown trace_breakdown{};
        WorkloadElapsedBreakdown trace_elapsed{};
        uint64_t trace_elapsed_ns = 0;
        uint64_t depth = 0, encl_lv = 0, buf_items = 0, padded_seg_size = 0, raw_seg_size = 0;
        uint64_t lsm_items_per_page = 0;

        {   // index lifetime scope: destroyed before backend cleanup below
        IndexT index = IndexRunner<IndexT>::create(workload_tree_writes, false);

        // Timed init replay (PUT only)
        uint64_t init_replay_cnt = 0;
        TimedReplayStats init_stats{};
        if (!init_path.empty() && exp_settings.init_size > 0) {

            if constexpr (enable_batch_progress) {
                init_stats = replay_ops_batched<OpVec>(
                    init_path,
                    exp_settings.init_size,
                    kReadBatch,
                    "exp1_workload_init_replay",
                    [&](const ParsedOp& op) {
                        if (op.type != OpType::Put) {
                            fail_fast("exp1_workload_init_replay", init_path,
                                      "unexpected op, expected PUT", nullptr);
                        }
                        insert_new(index, op.key, op.value);
                        ++init_replay_cnt;
                    },
                    [&](uint64_t processed, uint64_t total) {
                        print_batch_progress("exp1_workload_init_replay", processed, total);
                    });
            } else {
                init_stats = replay_ops_batched<OpVec>(
                    init_path,
                    exp_settings.init_size,
                    kReadBatch,
                    "exp1_workload_init_replay",
                    [&](const ParsedOp& op) {
                        if (op.type != OpType::Put) {
                            fail_fast("exp1_workload_init_replay", init_path,
                                      "unexpected op, expected PUT", nullptr);
                        }
                        insert_new(index, op.key, op.value);
                        ++init_replay_cnt;
                    });
            }
        }
        init_stats.elapsed_ns += finalize_h2o2ram_phase(index);
        init_s = static_cast<double>(init_stats.elapsed_ns) / 1e9;
        printf("Initialization replayed %llu operations.\n",
               static_cast<unsigned long long>(init_replay_cnt));

        // Timed trace replay
        uint64_t trace_replay_cnt = 0;
        TimedReplayStats trace_stats{};
        if constexpr (enable_batch_progress) {
            trace_stats = replay_ops_batched<OpVec>(
                trace_path,
                exp_settings.trace_size,
                kReadBatch,
                "exp1_workload_trace_replay",
                [&](const ParsedOp& op) {
                    apply_replay_trace_op_for_target(
                        index, op, trace_path, "exp1_workload_trace_replay",
                        trace_breakdown);
                    ++trace_replay_cnt;
                },
                [&](uint64_t processed, uint64_t total) {
                    print_batch_progress("exp1_workload_trace_replay", processed, total);
                });
        } else {
            trace_stats = replay_ops_batched<OpVec>(
                trace_path,
                exp_settings.trace_size,
                kReadBatch,
                "exp1_workload_trace_replay",
                [&](const ParsedOp& op) {
                    apply_replay_trace_op_for_target(
                        index, op, trace_path, "exp1_workload_trace_replay",
                        trace_breakdown);
                    ++trace_replay_cnt;
                });
        }

        printf("Trace replayed %llu operations.\n",
               static_cast<unsigned long long>(trace_replay_cnt));

        trace_elapsed_ns = trace_stats.elapsed_ns;
        trace_elapsed   = split_workload_elapsed(trace_elapsed_ns, trace_breakdown);
        trace_s         = static_cast<double>(trace_elapsed_ns) / 1e9;
        size_mb         = static_cast<double>(index.getByteSize()) / (1024.0 * 1024.0);
        bptree_size_mb  = bplus_tree_size_mb(index);
        depth           = index.level_count();
        encl_lv         = index.enclave_levels;
        buf_items       = index.buffer_real_count;
        {
            const auto seg_info = PaddedSegInfoGetter<IndexT>::get(index);
            padded_seg_size = seg_info.z_out;
            raw_seg_size = seg_info.z_raw;
            lsm_items_per_page = seg_info.lsm_items_per_page;
        }
        }  // index destroyed here: all NCVec pages freed back to g_DefaultBackend
        delete EM::Backend::g_DefaultBackend;
        EM::Backend::g_DefaultBackend = nullptr;

        printf("  Levels: %llu total, %llu in enclave\n",
               static_cast<unsigned long long>(depth),
               static_cast<unsigned long long>(encl_lv));
        print_buffer_and_segment_line(buf_items, padded_seg_size, raw_seg_size, lsm_items_per_page);
        if constexpr (kCollectTradeoffTiming) {
            printf("[Exp2/tradeoff] init=%.4fs trace=%.9fs writes=%.9fs queries=%.9fs "
                   "write_ops=%llu query_ops=%llu size=%.4f MB",
                   init_s,
                   trace_s,
                   static_cast<double>(trace_elapsed.write_ns) / 1e9,
                   static_cast<double>(trace_elapsed.query_ns) / 1e9,
                   static_cast<unsigned long long>(trace_breakdown.write_ops),
                   static_cast<unsigned long long>(trace_breakdown.query_ops),
                   size_mb);
        } else {
            printf("[Exp1/workload] init=%.4fs trace=%.4fs size=%.4f MB",
                   init_s, trace_s, size_mb);
        }
        if constexpr (requires(const IndexT& idx) { idx.getBPlusTreeByteSize(); }) {
            printf(" bptree_size=%.4f MB", bptree_size_mb);
        }
        printf("\n");
        return;

    } else if (files.mode == WorkloadFiles::Mode::Exp1WorkloadSingleTraceReplay) {
        using OpVec = typename IndexT::template NCVec<ParsedOp>;
        const uint64_t workload_tree_writes =
            exp1_workload_tree_capacity_writes<IndexT>(exp_settings);
        if (workload_tree_writes == UINT64_MAX) {
            fail_fast("exp1_workload_capacity", folder_path,
                      "tree capacity overflow", nullptr);
        }
        constexpr bool enable_batch_progress = kExp1WorkloadBatchProgress<IndexT>;

        // Hoist result variables outside the index scope so they survive
        // the index destructor and can be printed after cleanup.
        double   total_s = 0.0, size_mb = 0.0;
        double   bptree_size_mb = 0.0;
        WorkloadTimeBreakdown trace_breakdown{};
        WorkloadElapsedBreakdown trace_elapsed{};
        uint64_t trace_elapsed_ns = 0;
        uint64_t depth = 0, encl_lv = 0, buf_items = 0, padded_seg_size = 0, raw_seg_size = 0;
        uint64_t lsm_items_per_page = 0;

        {   // index lifetime scope: destroyed before backend cleanup below
        IndexT index = IndexRunner<IndexT>::create(workload_tree_writes, false);

        uint64_t trace_replay_cnt = 0;
        TimedReplayStats trace_stats{};
        if constexpr (enable_batch_progress) {
            trace_stats = replay_ops_batched<OpVec>(
                trace_path,
                exp_settings.trace_size,
                kReadBatch,
                "exp1_workload_trace_replay",
                [&](const ParsedOp& op) {
                    apply_replay_trace_op_for_target(
                        index, op, trace_path, "exp1_workload_trace_replay",
                        trace_breakdown);
                    ++trace_replay_cnt;
                },
                [&](uint64_t processed, uint64_t total) {
                    print_batch_progress("exp1_workload_trace_replay", processed, total);
                });
        } else {
            trace_stats = replay_ops_batched<OpVec>(
                trace_path,
                exp_settings.trace_size,
                kReadBatch,
                "exp1_workload_trace_replay",
                [&](const ParsedOp& op) {
                    apply_replay_trace_op_for_target(
                        index, op, trace_path, "exp1_workload_trace_replay",
                        trace_breakdown);
                    ++trace_replay_cnt;
                });
        }

        printf("Trace replayed %llu operations.\n",
               static_cast<unsigned long long>(trace_replay_cnt));

        trace_elapsed_ns = trace_stats.elapsed_ns;
        trace_elapsed   = split_workload_elapsed(trace_elapsed_ns, trace_breakdown);
        total_s         = static_cast<double>(trace_elapsed_ns) / 1e9;
        size_mb         = static_cast<double>(index.getByteSize()) / (1024.0 * 1024.0);
        bptree_size_mb  = bplus_tree_size_mb(index);
        depth           = index.level_count();
        encl_lv         = index.enclave_levels;
        buf_items       = index.buffer_real_count;
        {
            const auto seg_info = PaddedSegInfoGetter<IndexT>::get(index);
            padded_seg_size = seg_info.z_out;
            raw_seg_size = seg_info.z_raw;
            lsm_items_per_page = seg_info.lsm_items_per_page;
        }
        }  // index destroyed here: all NCVec pages freed back to g_DefaultBackend
        delete EM::Backend::g_DefaultBackend;
        EM::Backend::g_DefaultBackend = nullptr;

        printf("  Levels: %llu total, %llu in enclave\n",
               static_cast<unsigned long long>(depth),
               static_cast<unsigned long long>(encl_lv));
        print_buffer_and_segment_line(buf_items, padded_seg_size, raw_seg_size, lsm_items_per_page);
        if constexpr (kCollectTradeoffTiming) {
            printf("[Exp2/tradeoff] trace=%.9fs writes=%.9fs queries=%.9fs "
                   "write_ops=%llu query_ops=%llu size=%.4f MB",
                   total_s,
                   static_cast<double>(trace_elapsed.write_ns) / 1e9,
                   static_cast<double>(trace_elapsed.query_ns) / 1e9,
                   static_cast<unsigned long long>(trace_breakdown.write_ops),
                   static_cast<unsigned long long>(trace_breakdown.query_ops),
                   size_mb);
        } else {
            printf("[Exp1/workload] trace=%.4fs size=%.4f MB", total_s, size_mb);
        }
        if constexpr (requires(const IndexT& idx) { idx.getBPlusTreeByteSize(); }) {
            printf(" bptree_size=%.4f MB", bptree_size_mb);
        }
        printf("\n");
        return;

    } else {
        printf("[ERROR] Unsupported workload mode.\n");
        delete EM::Backend::g_DefaultBackend;
        EM::Backend::g_DefaultBackend = nullptr;
        return;
    }
}

template <IndexType IT, ExpType ET>
/**
 * @brief Resolve workload files/settings and dispatch to a concrete index runner.
 */
void run_test(const std::string& folder_name) {
    std::string folder_path;
    WorkloadFiles files;
    std::string data_size_label = "-";
    ExpSettings exp_settings{};
    
    if constexpr (ET == ExpType::Exp1_Scalability) {
        folder_path = std::string("../../benchmark/Exp1/scalability/") + folder_name + "/";
        files = resolve_exp1_workload_files(folder_path);
        char labelBuf[32];
        format_exp1_datasize_label(folder_name, labelBuf, sizeof(labelBuf));
        data_size_label = labelBuf;
        
        // Parse power from folder name (e.g., "YCSB_2^17" -> 17)
        unsigned int power = 0;
        if (ExpParse::ycsb_power(folder_name, power)) {
            exp_settings = Exp1ScalabilitySettings(power);
        } else {
            printf("[ERROR] Cannot parse power from folder name: %s\n", folder_name.c_str());
            return;
        }
    } else if constexpr (ET == ExpType::Exp1_Workload) {
        const std::string_view workload_name = folder_name;
        data_size_label = folder_name;
        if (workload_name == "ycsb_mix") {
            folder_path = "../../benchmark/Exp1/workload/ycsb/";
        } else if (workload_name == "ycsb_a" ||
                   workload_name == "ycsba") {
            folder_path = "../../benchmark/Exp1/workload/ycsb/";
        } else {
            folder_path = std::string("../../benchmark/Exp1/workload/") + folder_name + "/";
        }
        files = resolve_exp1_workload_replay_files(folder_path, folder_name);
        
        // Determine which Exp1/workload flavor based on folder name or trace path.
        if (files.trace_path.find("MetaKV") != std::string::npos ||
            workload_name.find("metakv") != std::string::npos) {
            exp_settings = Exp1WorkloadMetakvSettings();
        } else if (files.trace_path.find("IBMObjectStore") != std::string::npos ||
                   workload_name.find("ibm") != std::string::npos) {
            exp_settings = Exp1WorkloadIbmSettings();
        } else {
            // Default to YCSB replay settings for other workload names.
            exp_settings = Exp1WorkloadYcsbSettings();
        }
    } else {
        // Exp1/rangequery and Exp2/parameters have dedicated execution paths
        // that do not go through this generic run_test() helper.
        printf("[ERROR] Unsupported run_test ExpType for folder/workload path dispatch.\n");
        return;
    }

    if constexpr (IT == IndexType::Sapphmap) {
        using MapType = EM::Sapphmap::SapphMap<HardwareConfig::CachePageSize, HardwareConfig::EnclaveSize>;
        run_index_test_impl<MapType>(
            folder_path, files, data_size_label, "Sapphmap",
            /*support_static_mode=*/true, exp_settings);
    } else if constexpr (IT == IndexType::KVLsmtree) {
        using TreeType = EM::Sapphmap::KVLSMTree<HardwareConfig::CachePageSize, HardwareConfig::EnclaveSize>;
        run_index_test_impl<TreeType>(
            folder_path, files, data_size_label, "Nolsmtree",
            /*support_static_mode=*/false, exp_settings);
    } else if constexpr (IT == IndexType::NoHashMap) {
        using NoHashMapType = EM::Sapphmap::NoHashMap<HardwareConfig::CachePageSize, HardwareConfig::EnclaveSize>;
        run_index_test_impl<NoHashMapType>(
            folder_path, files, data_size_label, "NoHashMap",
            /*support_static_mode=*/false, exp_settings);
    } else if constexpr (IT == IndexType::H2O2RAM) {
        using H2O2RAMType = H2O2RAMAdapter<HardwareConfig::CachePageSize, HardwareConfig::EnclaveSize>;
        run_index_test_impl<H2O2RAMType>(
            folder_path, files, data_size_label, "H2O2RAM",
            /*support_static_mode=*/false, exp_settings);
    } else {
        using EnigMapType = EnigMapIndexAdapter<HardwareConfig::CachePageSize, HardwareConfig::EnclaveSize>;
        run_index_test_impl<EnigMapType>(
            folder_path, files, data_size_label, "EnigMap",
            /*support_static_mode=*/false, exp_settings);
    }
}

template <IndexType IT>
inline void run_exp1_scalability_subtest(const std::string& folder_name) {
    run_test<IT, ExpType::Exp1_Scalability>(folder_name);
}

template <IndexType IT>
inline void run_exp1_workload_subtest(const std::string& workload_name) {
    run_test<IT, ExpType::Exp1_Workload>(workload_name);
}

template <IndexType IT>
inline void run_exp1_subtest(const std::string& folder_name) {
    run_exp1_scalability_subtest<IT>(folder_name);
}

template <IndexType IT>
inline void run_exp1_workload_dispatch_subtest(const std::string& workload_name) {
    run_exp1_workload_subtest<IT>(workload_name);
}

template <IndexType IT>
inline void run_exp2_subtest(const std::string& workload_name) {
    run_exp1_workload_dispatch_subtest<IT>(workload_name);
}

inline void run_exp1_all_subtests() {
    static const char* kSizes[] = {
        "YCSB_2^17", "YCSB_2^18", "YCSB_2^19",
        "YCSB_2^20", "YCSB_2^21", "YCSB_2^22", "YCSB_2^23", "YCSB_2^24", "YCSB_2^25",
        "YCSB_2^26", "YCSB_2^27", "YCSB_2^28", "YCSB_2^29", "YCSB_2^30", "YCSB_2^31",
        "YCSB_2^32"
    };

    const uint64_t n = static_cast<uint64_t>(sizeof(kSizes) / sizeof(kSizes[0]));
    for (uint64_t i = 0; i < n; ++i) {
        run_exp1_scalability_subtest<IndexType::Sapphmap>(kSizes[i]);
        run_exp1_scalability_subtest<IndexType::KVLsmtree>(kSizes[i]);
    }
}

inline void run_exp1_workload_all_subtests() {
    static const char* kWorkloads[] = {
        "ycsb_a",
        "ycsb_mix",
        "metakv",
        "ibm"
    };

    const uint64_t n = static_cast<uint64_t>(sizeof(kWorkloads) / sizeof(kWorkloads[0]));
    for (uint64_t i = 0; i < n; ++i) {
        run_exp1_workload_subtest<IndexType::Sapphmap>(kWorkloads[i]);
        run_exp1_workload_subtest<IndexType::KVLsmtree>(kWorkloads[i]);
    }
}

inline void run_exp2_all_subtests() {
    run_exp1_workload_all_subtests();
}

// ============================================================================
// ============================================================================
//  Experiment organization aligned with the paper narrative:
//
//  Exp1 (core comparison):
//    - scalability: size sweep (handled via run_test + Exp1/scalability path)
//    - workload: ycsb/metakv/ibm replay (handled via run_test + Exp1/workload)
//    - rangequery: selectivity sweep (handled in run_exp1_rangequery)
//
//  Exp2 (parameter sensitivity):
//    - privacy    (legacy case a)
//    - fanout     (legacy case b)
//    - buffersize (legacy case c)
//    - blocksize  (legacy case e)
//
//  Workload for Exp2 parameters: YCSBInitData + YCSBGet (range phase skipped).
//  File ops use load_ops_batch (bulk ocall) as elsewhere in this header.
// ============================================================================

// ---------------------------------------------------------------------------
// BenchConfig: dynamic parameters for a single Exp2/Exp1-range run.
// ---------------------------------------------------------------------------
struct BenchConfig {
    double   eps_total;
    uint64_t gamma;
    uint64_t buffer_size;
    uint64_t init_size;
    // Filled by load_bench_common_cfg: min(kReadBatch, init_size).
    uint64_t read_batch = kReadBatch;
    uint64_t point_query_ops = kExp1QueryTraceOpCount;
    uint64_t range_query_ops = kExp1QueryTraceOpCount;
    uint64_t domain_size;
    /// When true, run_single_bench_test skips Phase 3 (range replay).
    /// Used by Exp2 privacy/fanout/buffersize/blocksize; Exp1/rangequery uses a dedicated path.
    bool skip_range_query = false;
};

// Returns a copy of the baseline default benchmark configuration.
inline BenchConfig kBenchDefault() { return {}; }

// ---------------------------------------------------------------------------
// BenchPaths: workload file paths for one dynamic benchmark run.
// range_path is only required when skip_range_query == false.
// ---------------------------------------------------------------------------
struct BenchPaths {
    std::string init_path;
    std::string get_path;
    std::string range_path;
};

// ---------------------------------------------------------------------------
// BenchResult: per-operation average timings returned by run_single_bench_test
// ---------------------------------------------------------------------------
struct BenchResult {
    double avg_insert_us = 0.0;
    double avg_point_us  = 0.0;
    double avg_range_ms  = 0.0;
    double size_mb       = 0.0;
    double bptree_size_mb = 0.0;
    bool   valid         = false;
};

struct BenchTopologyInfo {
    uint64_t total_depth     = 0;
    uint64_t enclave_levels  = 0;
    uint64_t buffer_items    = 0;
    uint64_t padded_seg_size = 0;
    uint64_t raw_seg_size    = 0;
    /// Non-zero for KVLSMTree: use items_per_page line instead of Z_out / Z_raw.
    uint64_t lsm_items_per_page = 0;
};

template <uint64_t CPS, uint64_t ES>
inline BenchTopologyInfo collect_bench_topology(const EM::Sapphmap::SapphMap<CPS, ES>& index) {
    BenchTopologyInfo topo;
    topo.total_depth    = index.level_count();
    topo.enclave_levels = index.enclave_levels;
    topo.buffer_items   = index.buffer_real_count;
    topo.padded_seg_size = 0;
    topo.raw_seg_size = 0;

    for (uint64_t lv = 1; lv <= topo.total_depth; ++lv) {
        const size_t idx = static_cast<size_t>(lv);
        if (idx >= index.levels.size()) break;
        const auto& ls = index.levels[idx];
        if (!ls.occupied) continue;
        if (ls.is_enclave && ls.enc_pds) {
            topo.padded_seg_size = ls.enc_pds->Z;
            topo.raw_seg_size = ls.enc_pds->Z_raw;
        } else if (ls.ext_pds) {
            topo.padded_seg_size = ls.ext_pds->Z;
            topo.raw_seg_size = ls.ext_pds->Z_raw;
        }
        break;
    }
    return topo;
}

template <uint64_t CPS, uint64_t ES>
inline BenchTopologyInfo collect_bench_topology(const EM::Sapphmap::KVLSMTree<CPS, ES>& index) {
    BenchTopologyInfo topo;
    topo.total_depth    = index.level_count();
    topo.enclave_levels = index.enclave_levels;
    topo.buffer_items   = index.buffer_real_count;
    topo.lsm_items_per_page = EM::Sapphmap::KVLSMTree<CPS, ES>::kItemsPerPage;
    return topo;
}

template <uint64_t CPS, uint64_t ES>
inline BenchTopologyInfo collect_bench_topology(const EnigMapIndexAdapter<CPS, ES>& index) {
    BenchTopologyInfo topo;
    topo.total_depth    = index.level_count();
    topo.enclave_levels = index.enclave_levels;
    topo.buffer_items   = index.buffer_real_count;
    return topo;
}

template <uint64_t CPS, uint64_t ES>
inline BenchTopologyInfo collect_bench_topology(const EM::Sapphmap::NoHashMap<CPS, ES>& index) {
    BenchTopologyInfo topo;
    topo.total_depth    = index.level_count();
    topo.enclave_levels = index.enclave_levels;
    topo.buffer_items   = index.buffer_real_count;
    return topo;
}

template <uint64_t CPS, uint64_t ES>
inline BenchTopologyInfo collect_bench_topology(const H2O2RAMAdapter<CPS, ES>& index) {
    BenchTopologyInfo topo;
    topo.total_depth = index.level_count();
    topo.enclave_levels = index.enclave_levels;
    topo.buffer_items = index.buffer_real_count;
    return topo;
}

// ---------------------------------------------------------------------------
// DynamicIndexRunner: construct SapphMap from BenchConfig (fully dynamic parameters)
//
// Unlike the Exp1/Exp2 IndexRunner which reads compile-time StructureParams,
// this variant forwards buffer_size, gamma, and eps_total from BenchConfig so
// that each sub-experiment can vary them independently.
// ---------------------------------------------------------------------------
template <typename IndexT>
struct DynamicIndexRunner;

template <uint64_t CPS, uint64_t ES>
struct DynamicIndexRunner<EM::Sapphmap::SapphMap<CPS, ES>> {
    using Index = EM::Sapphmap::SapphMap<CPS, ES>;

    static Index create(const BenchConfig& cfg) {
        const double delta_total =
            1.0 / (2.0 * std::pow(static_cast<double>(cfg.init_size), 1.2));
        return Index(cfg.buffer_size, cfg.gamma, cfg.init_size,
                     cfg.eps_total, delta_total, /*static_mode=*/false);
    }
};

template <uint64_t CPS, uint64_t ES>
struct DynamicIndexRunner<EM::Sapphmap::KVLSMTree<CPS, ES>> {
    using Index = EM::Sapphmap::KVLSMTree<CPS, ES>;

    static Index create(const BenchConfig& cfg) {
        return Index(cfg.buffer_size, cfg.gamma, cfg.init_size);
    }
};

template <uint64_t CPS, uint64_t ES>
struct DynamicIndexRunner<EnigMapIndexAdapter<CPS, ES>> {
    using Index = EnigMapIndexAdapter<CPS, ES>;

    static Index create(const BenchConfig& cfg) {
        const uint64_t max_writes = cfg.init_size * 2ULL + 1024ULL;
        return Index((max_writes == 0) ? 1ULL : max_writes);
    }
};

template <uint64_t CPS, uint64_t ES>
struct DynamicIndexRunner<EM::Sapphmap::NoHashMap<CPS, ES>> {
    using Index = EM::Sapphmap::NoHashMap<CPS, ES>;

    static Index create(const BenchConfig& cfg) {
        return Index(cfg.buffer_size, cfg.gamma, cfg.init_size);
    }
};

template <uint64_t CPS, uint64_t ES>
struct DynamicIndexRunner<H2O2RAMAdapter<CPS, ES>> {
    using Index = H2O2RAMAdapter<CPS, ES>;
    static Index create(const BenchConfig& cfg) {
        return Index((cfg.init_size == 0) ? 1ULL : cfg.init_size);
    }
};

// ---------------------------------------------------------------------------
// run_single_bench_test<IndexT>
//
// Mirrors Exp1 Phase 2: sequential build (batched bulk read) + point + optional range.
// Timers wrap index ops only; load_ops_batch is outside ocall_measure_time.
// ---------------------------------------------------------------------------
template <typename IndexT>
/**
 * @brief Execute one Exp2-parameter/Exp1-rangequery style run with dynamic knobs.
 */
BenchResult run_single_bench_test(const BenchConfig& cfg,
                                const BenchPaths&  paths,
                                const char*       label) {
    using Item  = EM::Definition::Item;
    using OpVec = typename IndexT::template NCVec<ParsedOp>;

    if (EM::Backend::g_DefaultBackend) {
        delete EM::Backend::g_DefaultBackend;
        EM::Backend::g_DefaultBackend = nullptr;
    }

    EM::Definition::DOMAIN_SIZE_D = cfg.domain_size;

    const uint64_t backend_items = cfg.init_size + 1;
    EM::Backend::g_DefaultBackend =
        new EM::Backend::MemServerBackend(
            8 * sizeof(Item)     * backend_items +
            8 * sizeof(uint64_t) * backend_items);

    BenchResult result;

    {
        IndexT index = DynamicIndexRunner<IndexT>::create(cfg);

        // --- Phase 1: Sequential build (batched bulk read) ---
        {
            const TimedReplayStats build_stats = replay_ops_batched<OpVec>(
                paths.init_path,
                cfg.init_size,
                cfg.read_batch,
                "bench_build",
                [&](const ParsedOp& op) {
                    if (op.type != OpType::Put) {
                        fail_fast("bench_build", paths.init_path,
                                  "expected PUT", nullptr);
                    }
                    index.Insert(op.key, op.value);
                });

            result.avg_insert_us = (build_stats.processed_ops == 0)
                ? 0.0
                : static_cast<double>(build_stats.elapsed_ns) / 1e3 /
                      static_cast<double>(build_stats.processed_ops);
        }

        // --- Phase 2: Point queries (single bulk load) ---
        {
            const std::string& gp = paths.get_path;

            const TimedReplayStats point_stats = replay_ops_single_batch<OpVec>(
                gp,
                cfg.point_query_ops,
                "bench_point",
                [&](const ParsedOp& op) {
                    if (op.type != OpType::Get) {
                        fail_fast("bench_point", gp, "expected GET", nullptr);
                    }
                    index.PointQuery(op.key);
                });

            result.avg_point_us = (point_stats.processed_ops == 0)
                ? 0.0
                : static_cast<double>(point_stats.elapsed_ns) / 1e3 /
                      static_cast<double>(point_stats.processed_ops);
        }

        if (!cfg.skip_range_query) {
            // --- Phase 3: Range queries (single bulk load) ---
            {
                if (paths.range_path.empty()) {
                    fail_fast("bench_range", paths.init_path,
                              "range_path is empty while range phase is enabled", nullptr);
                }
                const std::string& sp = paths.range_path;

                const TimedReplayStats range_stats = replay_ops_single_batch<OpVec>(
                    sp,
                    cfg.range_query_ops,
                    "bench_range",
                    [&](const ParsedOp& op) {
                        if (op.type != OpType::Scan) {
                            fail_fast("bench_range", sp, "expected SCAN", nullptr);
                        }
                        index.RangeQuery(op.key, op.right);
                    });

                result.avg_range_ms = (range_stats.processed_ops == 0)
                    ? 0.0
                    : static_cast<double>(range_stats.elapsed_ns) / 1e6 /
                          static_cast<double>(range_stats.processed_ops);
            }
        }

        result.size_mb        = static_cast<double>(index.getByteSize()) / (1024.0 * 1024.0);
        result.bptree_size_mb = bplus_tree_size_mb(index);
        result.valid          = true;

        const BenchTopologyInfo topo = collect_bench_topology(index);
        printf("[%s]\n", label);
        printf("  Levels: %llu total, %llu in enclave\n",
               static_cast<unsigned long long>(topo.total_depth),
               static_cast<unsigned long long>(topo.enclave_levels));
        print_buffer_and_segment_line(topo.buffer_items,
                                      topo.padded_seg_size,
                                      topo.raw_seg_size,
                                      topo.lsm_items_per_page);
    }

    delete EM::Backend::g_DefaultBackend;
    EM::Backend::g_DefaultBackend = nullptr;

    return result;
}

// ============================================================================
// Exp2/privacy: Vary Privacy Budget (EPS_TOTAL)
//   Folder : ../../benchmark/Exp2/privacy/YCSB_2^<BENCH_DATASET_POW>/
// ============================================================================
namespace BenchEnv {

inline bool load_bench_common_cfg(BenchConfig& cfg, unsigned int& pow) {
    cfg.eps_total   = static_cast<double>(EPS_TOTAL);
    cfg.gamma       = static_cast<uint64_t>(GAMMA);
    cfg.buffer_size = static_cast<uint64_t>(BUFFER_SIZE);
    pow             = static_cast<unsigned int>(BENCH_DATASET_POW);
    if (pow > 62U) {
        printf("[ERROR] invalid BENCH_DATASET_POW=%u (must <= 62)\n", pow);
        return false;
    }
    cfg.init_size         = (1ULL << pow);
    cfg.domain_size       = 100ULL * cfg.init_size;
    cfg.read_batch        = std::min(kReadBatch, cfg.init_size);
    cfg.point_query_ops   = kExp1QueryTraceOpCount;
    cfg.range_query_ops   = kExp1QueryTraceOpCount;
    cfg.skip_range_query  = true;  // Exp2 parameter tests: no range replay in run_single_bench_test.
    return true;
}

inline bool load_exp2_blocksize_cfg(BenchConfig& cfg, unsigned int& pow) {
    return load_bench_common_cfg(cfg, pow);
}

inline uint64_t get_block_size_from_macro() {
    return static_cast<uint64_t>(BLOCK_SIZE);
}

inline std::string get_bench_target_from_macro() {
    return std::string(BENCH_TARGET_STR);
}

inline std::string ycsb_folder(const std::string& base, unsigned int pow) {
    char sub[48];
    snprintf(sub, sizeof(sub), "YCSB_2^%u/", pow);
    return base + sub;
}

} // namespace BenchEnv

inline void run_exp2_privacy() {
    printf("========== Exp2/privacy: Vary Privacy Budget (EPS_TOTAL) ==========\n");
    unsigned int pow = 0;
    BenchConfig cfg = kBenchDefault();
    if (!BenchEnv::load_bench_common_cfg(cfg, pow)) return;

    const std::string folder = BenchEnv::ycsb_folder(
        "../../benchmark/Exp2/privacy/", pow);
    const BenchPaths paths {
        folder + "YCSBInitData.txt",
        folder + "YCSBGet.txt",
        ""
    };

    using MapType = EM::Sapphmap::SapphMap<HardwareConfig::CachePageSize,
                                           HardwareConfig::EnclaveSize>;

    char label[64], bufSz[32], blkSz[32];
    snprintf(label, sizeof(label), "Exp2/privacy eps=%.4f", cfg.eps_total);
    fmtSize(cfg.buffer_size, bufSz, sizeof(bufSz));
    fmtSize(static_cast<uint64_t>(HardwareConfig::CachePageSize), blkSz, sizeof(blkSz));
    printf("[Exp2/privacy] eps=%.4f gamma=%llu buffer=%s block=%s data=2^%u (sel=N/A, range phase off)\n",
           cfg.eps_total,
           static_cast<unsigned long long>(cfg.gamma),
           bufSz,
           blkSz,
           pow);
    const BenchResult r = run_single_bench_test<MapType>(cfg, paths, label);
    printf("[Exp2/privacy] ins=%.4f us/op, pq=%.4f us/op, size=%.4f MB "
           "bptree_size=%.4f MB (rq skipped)\n",
           r.avg_insert_us, r.avg_point_us, r.size_mb, r.bptree_size_mb);
}

// ============================================================================
// Exp2/fanout: Vary Fan-out (GAMMA)
//   Folder : ../../benchmark/Exp2/fanout/YCSB_2^<BENCH_DATASET_POW>/
// ============================================================================
inline void run_exp2_fanout() {
    printf("========== Exp2/fanout: Vary Fan-out (GAMMA) ==========\n");
    unsigned int pow = 0;
    BenchConfig cfg = kBenchDefault();
    if (!BenchEnv::load_bench_common_cfg(cfg, pow)) return;

    const std::string folder = BenchEnv::ycsb_folder(
        "../../benchmark/Exp2/fanout/", pow);
    const BenchPaths paths {
        folder + "YCSBInitData.txt",
        folder + "YCSBGet.txt",
        ""
    };

    using MapType = EM::Sapphmap::SapphMap<HardwareConfig::CachePageSize,
                                           HardwareConfig::EnclaveSize>;

    char label[64], bufSz[32], blkSz[32];
    snprintf(label, sizeof(label), "Exp2/fanout gamma=%llu",
             static_cast<unsigned long long>(cfg.gamma));
    fmtSize(cfg.buffer_size, bufSz, sizeof(bufSz));
    fmtSize(static_cast<uint64_t>(HardwareConfig::CachePageSize), blkSz, sizeof(blkSz));
    printf("[Exp2/fanout] eps=%.4f gamma=%llu buffer=%s block=%s data=2^%u (sel=N/A, range phase off)\n",
           cfg.eps_total,
           static_cast<unsigned long long>(cfg.gamma),
           bufSz,
           blkSz,
           pow);
    const BenchResult r = run_single_bench_test<MapType>(cfg, paths, label);
    printf("[Exp2/fanout] ins=%.4f us/op, pq=%.4f us/op, size=%.4f MB "
           "bptree_size=%.4f MB (rq skipped)\n",
           r.avg_insert_us, r.avg_point_us, r.size_mb, r.bptree_size_mb);
}

// ============================================================================
// Exp2/buffersize: Vary Buffer Size
//   Folder : ../../benchmark/Exp2/buffersize/YCSB_2^<BENCH_DATASET_POW>/
// ============================================================================
inline void run_exp2_buffersize() {
    printf("========== Exp2/buffersize: Vary Buffer Size ==========\n");
    unsigned int pow = 0;
    BenchConfig cfg = kBenchDefault();
    if (!BenchEnv::load_bench_common_cfg(cfg, pow)) return;

    const std::string folder = BenchEnv::ycsb_folder(
        "../../benchmark/Exp2/buffersize/", pow);
    const BenchPaths paths {
        folder + "YCSBInitData.txt",
        folder + "YCSBGet.txt",
        ""
    };

    using MapType = EM::Sapphmap::SapphMap<HardwareConfig::CachePageSize,
                                           HardwareConfig::EnclaveSize>;

    char label[64], bufSz[32], blkSz[32];
    fmtSize(cfg.buffer_size, bufSz, sizeof(bufSz));
    fmtSize(static_cast<uint64_t>(HardwareConfig::CachePageSize), blkSz, sizeof(blkSz));
    snprintf(label, sizeof(label), "Exp2/buffersize buf=%s", bufSz);
    printf("[Exp2/buffersize] eps=%.4f gamma=%llu buffer=%s block=%s data=2^%u (sel=N/A, range phase off)\n",
           cfg.eps_total,
           static_cast<unsigned long long>(cfg.gamma),
           bufSz,
           blkSz,
           pow);
    const BenchResult r = run_single_bench_test<MapType>(cfg, paths, label);
    printf("[Exp2/buffersize] ins=%.4f us/op, pq=%.4f us/op, size=%.4f MB "
           "bptree_size=%.4f MB (rq skipped)\n",
           r.avg_insert_us, r.avg_point_us, r.size_mb, r.bptree_size_mb);
}

// ============================================================================
// Exp2/payload: Vary Payload Size (PAYLOAD)
//   Folder : ../../benchmark/Exp2/payload/YCSB_2^<BENCH_DATASET_POW>/
// ============================================================================
inline void run_exp2_payload() {
    printf("========== Exp2/payload: Vary Payload Size (PAYLOAD) ==========\n");
    unsigned int pow = 0;
    BenchConfig cfg = kBenchDefault();
    if (!BenchEnv::load_bench_common_cfg(cfg, pow)) return;

    const std::string folder = BenchEnv::ycsb_folder(
        "../../benchmark/Exp2/payload/", pow);
    const BenchPaths paths {
        folder + "YCSBInitData.txt",
        folder + "YCSBGet.txt",
        ""
    };

    using MapType = EM::Sapphmap::SapphMap<HardwareConfig::CachePageSize,
                                           HardwareConfig::EnclaveSize>;

    char label[64], bufSz[32], blkSz[32];
    fmtSize(cfg.buffer_size, bufSz, sizeof(bufSz));
    fmtSize(static_cast<uint64_t>(HardwareConfig::CachePageSize), blkSz, sizeof(blkSz));
    snprintf(label, sizeof(label), "Exp2/payload payload=%luB",
             static_cast<unsigned long>(sizeof(MapType::Item) - 8));
    printf("[Exp2/payload] eps=%.4f gamma=%llu buffer=%s block=%s data=2^%u payload=%lu\n",
           cfg.eps_total,
           static_cast<unsigned long long>(cfg.gamma),
           bufSz,
           blkSz,
           pow,
           static_cast<unsigned long>(sizeof(MapType::Item) - 8));
    const BenchResult r = run_single_bench_test<MapType>(cfg, paths, label);
    printf("[Exp2/payload] ins=%.4f us/op, pq=%.4f us/op, size=%.4f MB "
           "bptree_size=%.4f MB (rq skipped)\n",
           r.avg_insert_us, r.avg_point_us, r.size_mb, r.bptree_size_mb);
}

// ============================================================================
// Exp1/rangequery: Vary Range Query Selectivity
//   Folder : ../../benchmark/Exp1/rangequery/YCSB_2^<pow>/
//   Init fixed; point replay skipped; Scan file varies: YCSBScan_<token>.txt
//     token in {1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8}
// ============================================================================
/**
 * @brief Run Exp1 rangequery with fixed build only, then sweep scan selectivities.
 */
inline void run_exp1_rangequery() {
    printf("========== Exp1/rangequery: Vary Range Query Selectivity ==========\n");
    unsigned int pow = 0;
    BenchConfig cfg = kBenchDefault();
    if (!BenchEnv::load_bench_common_cfg(cfg, pow)) return;

    const std::string folder = BenchEnv::ycsb_folder(
        "../../benchmark/Exp1/rangequery/", pow);
    const std::string init_path = folder + "YCSBInitData.txt";

#if BENCH_USE_ENIGMAP
    using MapType = EnigMapIndexAdapter<HardwareConfig::CachePageSize,
                                        HardwareConfig::EnclaveSize>;
    printf("[Exp1/rangequery] index=enigmap (range is emulated via point queries)\n");
#elif BENCH_USE_NOLSMTREE
    using MapType = EM::Sapphmap::KVLSMTree<HardwareConfig::CachePageSize,
                                            HardwareConfig::EnclaveSize>;
    printf("[Exp1/rangequery] index=nolsmtree (build: BENCH_INDEX=nolsmtree)\n");
#elif BENCH_USE_NOHASHMAP
    using MapType = EM::Sapphmap::NoHashMap<HardwareConfig::CachePageSize,
                                                HardwareConfig::EnclaveSize>;
    printf("[Exp1/rangequery] index=nohashmap (build: BENCH_INDEX=nohashmap)\n");
#elif BENCH_USE_H2O2RAM
    using MapType = H2O2RAMAdapter<HardwareConfig::CachePageSize,
                                  HardwareConfig::EnclaveSize>;
    printf("[Exp1/rangequery] index=h2o2ram (range is emulated via point queries)\n");
#else
    using MapType = EM::Sapphmap::SapphMap<HardwareConfig::CachePageSize,
                                          HardwareConfig::EnclaveSize>;
    printf("[Exp1/rangequery] index=sapphmap (build: BENCH_INDEX=sapphmap)\n");
#endif
    using OpVec = typename MapType::template NCVec<ParsedOp>;

    static const char* const kSelectLabels[] = {
        "1e2", "1e3", "1e4", "1e5", "1e6", "1e7", "1e8"
    };
    static constexpr uint64_t kNumSelectivity =
        sizeof(kSelectLabels) / sizeof(kSelectLabels[0]);

    if (EM::Backend::g_DefaultBackend) {
        delete EM::Backend::g_DefaultBackend;
        EM::Backend::g_DefaultBackend = nullptr;
    }
    EM::Definition::DOMAIN_SIZE_D = cfg.domain_size;
    const Exp1ScalabilitySettings backend_settings(static_cast<uint64_t>(pow));
    const uint64_t backend_bytes = compute_backend_size_bytes<MapType>(
        backend_settings,
        WorkloadFiles::Mode::Exp1ScalabilitySplit);
    if (backend_bytes == UINT64_MAX) {
        fail_fast("exp1_rangequery_backend", folder,
                  "backend size overflow", nullptr);
    }
    EM::Backend::g_DefaultBackend =
        new EM::Backend::MemServerBackend(backend_bytes);
    double avg_insert_us = 0.0;
    double size_mb       = 0.0;
    double bptree_size_mb = 0.0;
    uint64_t topo_depth = 0, topo_enc = 0, topo_buf = 0, topo_z = 0, topo_z_raw = 0;

    {
        MapType index = DynamicIndexRunner<MapType>::create(cfg);
        // Build once
        {
            const TimedReplayStats build_stats = replay_ops_batched<OpVec>(
                init_path,
                cfg.init_size,
                cfg.read_batch,
                "exp1_range_build",
                [&](const ParsedOp& op) {
                    if (op.type != OpType::Put) {
                        fail_fast("exp1_range_build", init_path, "expected PUT", nullptr);
                    }
                    index.Insert(op.key, op.value);
                });

            avg_insert_us = (build_stats.processed_ops == 0)
                ? 0.0
                : static_cast<double>(build_stats.elapsed_ns) / 1e3 /
                      static_cast<double>(build_stats.processed_ops);
        }

        size_mb = static_cast<double>(index.getByteSize()) / (1024.0 * 1024.0);
        bptree_size_mb = bplus_tree_size_mb(index);
        const BenchTopologyInfo topo = collect_bench_topology(index);
        topo_depth = topo.total_depth;
        topo_enc   = topo.enclave_levels;
        topo_buf   = topo.buffer_items;
        topo_z     = topo.padded_seg_size;
        topo_z_raw = topo.raw_seg_size;

        char bufSz[32], blkSz[32];
        fmtSize(cfg.buffer_size, bufSz, sizeof(bufSz));
        fmtSize(static_cast<uint64_t>(HardwareConfig::CachePageSize), blkSz, sizeof(blkSz));

        printf("  Levels: %llu total, %llu in enclave\n",
               static_cast<unsigned long long>(topo_depth),
               static_cast<unsigned long long>(topo_enc));
        print_buffer_and_segment_line(topo_buf, topo_z, topo_z_raw,
                                      topo.lsm_items_per_page);
           printf("[Exp1/rangequery] eps=%.4f gamma=%llu buffer=%s block=%s data=2^%u\n",
               cfg.eps_total,
               static_cast<unsigned long long>(cfg.gamma),
               bufSz,
               blkSz,
               pow);
           printf("[Exp1/rangequery] ins=%.4f us/op, size=%.4f MB "
                  "bptree_size=%.4f MB (pq skipped)\n",
               avg_insert_us, size_mb, bptree_size_mb);

        for (uint64_t si = 0; si < kNumSelectivity; ++si) {
            const std::string scan_path = folder + "YCSBScan_" + kSelectLabels[si] + ".txt";

            const TimedReplayStats range_stats = replay_ops_single_batch<OpVec>(
                scan_path,
                cfg.range_query_ops,
                "exp1_range_range",
                [&](const ParsedOp& op) {
                    if (op.type != OpType::Scan) {
                        fail_fast("exp1_range_range", scan_path, "expected SCAN", nullptr);
                    }
                    index.RangeQuery(op.key, op.right);
                });

            const double avg_range_ms = (range_stats.processed_ops == 0)
                ? 0.0
                : static_cast<double>(range_stats.elapsed_ns) / 1e6 /
                      static_cast<double>(range_stats.processed_ops);

            printf("  sel=%s, rq=%.4f ms/op\n", kSelectLabels[si], avg_range_ms);
        }
    }

    delete EM::Backend::g_DefaultBackend;
    EM::Backend::g_DefaultBackend = nullptr;
}

// ============================================================================
// Exp2/blocksize: Vary Block Size x Data Size (single run per process)
//
// Workload per BENCH_DATASET_POW: blocksize/YCSB_2^<pow>/
//   YCSBInitData + YCSBGet (same as Exp2 privacy/fanout/buffersize: no range replay file)
// ============================================================================

// Dispatch to SapphMap<CPS, ES> by runtime cps_value.
// ES is fixed at HardwareConfig::EnclaveSize (compile-time constant).
template <uint64_t ES>
inline void exp2_blocksize_dispatch_one(uint64_t          cps_value,
                               const BenchConfig& cfg,
                               const BenchPaths&  paths,
                               const char*       label,
                               BenchResult&       out) {
    switch (cps_value) {
        case 1024ULL:
            out = run_single_bench_test<EM::Sapphmap::SapphMap<1024ULL,  ES>>(cfg, paths, label);
            break;
        case 2048ULL:
            out = run_single_bench_test<EM::Sapphmap::SapphMap<2048ULL,  ES>>(cfg, paths, label);
            break;
        case 4096ULL:
            out = run_single_bench_test<EM::Sapphmap::SapphMap<4096ULL,  ES>>(cfg, paths, label);
            break;
        case 8192ULL:
            out = run_single_bench_test<EM::Sapphmap::SapphMap<8192ULL,  ES>>(cfg, paths, label);
            break;
        case 16384ULL:
            out = run_single_bench_test<EM::Sapphmap::SapphMap<16384ULL, ES>>(cfg, paths, label);
            break;
        case 32768ULL:
            out = run_single_bench_test<EM::Sapphmap::SapphMap<32768ULL, ES>>(cfg, paths, label);
            break;
        case 65536ULL:
            out = run_single_bench_test<EM::Sapphmap::SapphMap<65536ULL, ES>>(cfg, paths, label);
            break;
        case 131072ULL:
            out = run_single_bench_test<EM::Sapphmap::SapphMap<131072ULL, ES>>(cfg, paths, label);
            break;
        case 262144ULL:
            out = run_single_bench_test<EM::Sapphmap::SapphMap<262144ULL, ES>>(cfg, paths, label);
            break;
        default:
            printf("[ERROR] Exp2/blocksize: unsupported block size %llu\n",
                   static_cast<unsigned long long>(cps_value));
            break;
    }
}

inline void run_exp2_blocksize() {
    printf("========== Exp2/blocksize: Single Run (Block Size x Data Size) ==========\n");

    unsigned int pow = 0;
    BenchConfig cfg = kBenchDefault();
    if (!BenchEnv::load_exp2_blocksize_cfg(cfg, pow)) return;

    const uint64_t cps = BenchEnv::get_block_size_from_macro();

    const std::string folder = BenchEnv::ycsb_folder(
        "../../benchmark/Exp2/blocksize/", pow);
    const BenchPaths paths {
        folder + "YCSBInitData.txt",
        folder + "YCSBGet.txt",
        ""
    };

    char label[96], bufSz[32], blkSz[32];
    fmtSize(cfg.buffer_size, bufSz, sizeof(bufSz));
    fmtSize(cps, blkSz, sizeof(blkSz));
    snprintf(label, sizeof(label), "Exp2/blocksize 2^%u blk=%s", pow, blkSz);

    printf("[Exp2/blocksize] eps=%.4f gamma=%llu buffer=%s data=2^%u block=%s (sel=N/A, range phase off)\n",
           cfg.eps_total,
           static_cast<unsigned long long>(cfg.gamma),
           bufSz,
           pow,
           blkSz);
    BenchResult r;
    exp2_blocksize_dispatch_one<HardwareConfig::EnclaveSize>(cps, cfg, paths, label, r);
    printf("[Exp2/blocksize] ins=%.4f us/op, pq=%.4f us/op, size=%.4f MB "
           "bptree_size=%.4f MB (rq skipped)\n",
           r.avg_insert_us, r.avg_point_us, r.size_mb, r.bptree_size_mb);
}

// ============================================================================
// Top-level benchmark dispatcher by BENCH_TARGET.
// ============================================================================
/**
 * @brief Top-level BENCH_TARGET dispatcher used by run.sh / run_exp.sh.
 */
inline void run_bench_target() {
    const std::string target = BenchEnv::get_bench_target_from_macro();
    if (target == "exp1_scalability") {
        std::string folder = std::string(BENCH_EXP1_FOLDER_STR);
        if (folder.empty()) {
            char folder_buf[48];
            snprintf(folder_buf, sizeof(folder_buf), "YCSB_2^%u",
                     static_cast<unsigned int>(BENCH_DATASET_POW));
            folder = folder_buf;
        }
#if BENCH_USE_ENIGMAP
        run_exp1_subtest<IndexType::EnigMap>(folder);
#elif BENCH_USE_NOLSMTREE
        run_exp1_subtest<IndexType::KVLsmtree>(folder);
#elif BENCH_USE_NOHASHMAP
        run_exp1_subtest<IndexType::NoHashMap>(folder);
#elif BENCH_USE_H2O2RAM
        run_exp1_subtest<IndexType::H2O2RAM>(folder);
#else
        run_exp1_subtest<IndexType::Sapphmap>(folder);
#endif
    } else if (target == "exp1_workload" || target == "exp2_tradeoff") {
        const std::string workload = std::string(BENCH_WORKLOAD_STR);
#if BENCH_USE_ENIGMAP
        run_exp1_workload_dispatch_subtest<IndexType::EnigMap>(workload);
#elif BENCH_USE_NOLSMTREE
        run_exp1_workload_dispatch_subtest<IndexType::KVLsmtree>(workload);
#elif BENCH_USE_NOHASHMAP
        run_exp1_workload_dispatch_subtest<IndexType::NoHashMap>(workload);
#elif BENCH_USE_H2O2RAM
        run_exp1_workload_dispatch_subtest<IndexType::H2O2RAM>(workload);
#else
        run_exp1_workload_dispatch_subtest<IndexType::Sapphmap>(workload);
#endif
    } else if (target == "exp1_rangequery") {
        run_exp1_rangequery();
    } else if (target == "exp2_privacy") {
        run_exp2_privacy();
    } else if (target == "exp2_fanout") {
        run_exp2_fanout();
    } else if (target == "exp2_buffersize") {
        run_exp2_buffersize();
    } else if (target == "exp2_blocksize") {
        run_exp2_blocksize();
    } else if (target == "exp2_payload") {
        run_exp2_payload();
    } else {
        printf("[ERROR] Unsupported BENCH_TARGET=%s in run_bench_target()\n", target.c_str());
    }
}

// ============================================================================
// ============================================================================

}  // namespace Experiment
