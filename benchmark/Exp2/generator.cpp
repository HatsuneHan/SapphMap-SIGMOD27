#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <sstream>
#include <filesystem>
#include <cstdio>
#include <cstdint>
#include <cctype>
#include <cstdlib>
#include <algorithm>
#include <unordered_set>
#include <random>

#include "../ycsb_output.hpp"

namespace fs = std::filesystem;

const std::string EXP2_BLOCKSIZE_DIR = "./blocksize/";
const std::string EXP2_PRIVACY_DIR = "./privacy/";
const std::string EXP2_FANOUT_DIR = "./fanout/";
const std::string EXP2_BUFFERSIZE_DIR = "./buffersize/";
const std::string EXP2_PAYLOAD_DIR = "./payload/";
const std::string EXP1_RANGEQUERY_DIR = "../Exp1/rangequery/";

const std::string YCSB_HOME_REL = "../dependency/ycsb-0.17.0";
const std::unordered_set<std::string> VALID_OPS = {"INSERT", "READ", "UPDATE", "SCAN"};

const uint64_t OPERATION_COUNT = 65536;
const int BLOCKSIZE_MIN_POWER = 19;
const int BLOCKSIZE_MAX_POWER = 26;
const int SHARED_DATA_POWER = 24;

// Java-side safe integer bound used by YCSB parameter parsing.
const uint64_t BATCH_SIZE_LIMIT = 1ULL << 30;

std::string get_ycsb_classpath(const fs::path& ycsb_home) {
    if (!fs::exists(ycsb_home)) {
        std::cerr << "\n[Fatal Error] YCSB home directory does not exist: " << ycsb_home << "\n";
        exit(1);
    }
    std::vector<std::string> cp = { (ycsb_home / "conf").string() };
    fs::path core_lib = ycsb_home / "lib";
    if (fs::exists(core_lib)) cp.push_back((core_lib / "*").string());

    fs::path basic_lib_1 = ycsb_home / "basic-binding" / "lib";
    fs::path basic_lib_2 = ycsb_home / "bindings" / "basic" / "lib";
    if (fs::exists(basic_lib_1)) cp.push_back((basic_lib_1 / "*").string());
    else if (fs::exists(basic_lib_2)) cp.push_back((basic_lib_2 / "*").string());

    std::string classpath = "";
    for (size_t i = 0; i < cp.size(); ++i) {
        classpath += cp[i];
#ifdef _WIN32
        if (i < cp.size() - 1) classpath += ";";
#else
        if (i < cp.size() - 1) classpath += ":";
#endif
    }
    return classpath;
}

uint64_t parse_ycsb_key(const std::string& key_str) {
    std::string digits;
    for (char c : key_str) if (std::isdigit(c)) digits += c;
    return digits.empty() ? 0 : std::stoull(digits);
}

// Keep the key-domain mapping stable across all generated traces.
uint64_t deterministic_hash_map(uint64_t raw_id, uint64_t domain_size) {
    return (raw_id * 11400714819323198483ULL % domain_size) + 1;
}

uint64_t generate_value(uint64_t raw_id) {
    return (raw_id * 6364136223846793005ULL + 1442695040888963407ULL) & 0xFFFFFFFFFFFFFFFFULL;
}

// Build one YCSB workload profile file.
void create_workload_file(const fs::path& wk_path, const std::string& proportions, bool is_load_phase, uint64_t max_scan_length = 10000) {
    fs::create_directories(wk_path.parent_path());
    std::ofstream f(wk_path);
    if (!f.is_open()) {
        std::cerr << "[Path Error] Failed to create workload file: " << wk_path << "\n";
        exit(1);
    }
    
    // Use ordered inserts in load phase to keep deterministic append behavior.
    std::string insert_order = is_load_phase ? "ordered" : "hashed";

    f << "workload=site.ycsb.workloads.CoreWorkload\n"
      << "requestdistribution=zipfian\n"
      << "zipfianconstant=0.99\n"
      << "maxscanlength=" << max_scan_length << "\n"
      << "scanlengthdistribution=uniform\n"
      << "insertorder=" << insert_order << "\n" 
      << "fieldcount=1\n"
      << "fieldlength=8\n"
      << proportions;
    f.close();
}

// Generate one load batch and append converted PUT records to output.
bool run_ycsb_batch_load(const fs::path& ycsb_home, const std::string& wk_name,
                         const fs::path& out_path, uint64_t local_record_count,
                         uint64_t base_offset, uint64_t domain_size) {
    
    std::string classpath = get_ycsb_classpath(ycsb_home);
    fs::path wk_path = ycsb_home / "workloads" / wk_name;
    
    std::ofstream f(out_path, std::ios_base::app);
    if (!f.is_open()) {
        std::cerr << "[Error] Failed to open output file for load: " << out_path << "\n";
        return false;
    }

    std::stringstream cmd_ss;
    cmd_ss << "java -Xmx8g -cp \"" << classpath << "\" site.ycsb.Client -load " 
           << " -db site.ycsb.BasicDB -P " << wk_path.string() 
           << " -p recordcount=" << local_record_count 
           << " -p insertstart=0" 
           << " -p insertcount=" << local_record_count 
           << " -p operationcount=0" 
           << " -s"; 

    std::string cmd = cmd_ss.str();

#ifdef _WIN32
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif

    if (!pipe) {
        return BenchmarkGeneration::finish_ycsb_output(
            pipe, f, out_path, 0, local_record_count);
    }

    char buffer[1024];
    uint64_t count = 0;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        std::istringstream iss(buffer);
        std::string op, table, key_str;
        
        if (iss >> op >> table >> key_str && op == "INSERT") {
            try {
                uint64_t relative_id = parse_ycsb_key(key_str);
                uint64_t raw_id = relative_id + base_offset;
                uint64_t key = deterministic_hash_map(raw_id, domain_size);
                f << "PUT " << key << " " << generate_value(raw_id) << "\n";
                ++count;
            } catch (...) { continue; }
        }
    }

    return BenchmarkGeneration::finish_ycsb_output(
        pipe, f, out_path, count, local_record_count);
}

// Run YCSB and export only the requested operation projection (GET or SCAN).
bool run_ycsb_run_phase(const fs::path& ycsb_home, const std::string& wk_name,
                        const fs::path& out_path, uint64_t total_record_count,
                        uint64_t target_count, uint64_t domain_size,
                        uint64_t max_scan_len_cap = 10000) {
    
    uint64_t ycsb_record_count = total_record_count;
    uint64_t scale_factor = 1;
    
    if (total_record_count > BATCH_SIZE_LIMIT) {
        ycsb_record_count = BATCH_SIZE_LIMIT;
        scale_factor = total_record_count / BATCH_SIZE_LIMIT; 
    }

    std::string classpath = get_ycsb_classpath(ycsb_home);
    fs::path wk_path = ycsb_home / "workloads" / wk_name;
    std::ofstream f(out_path);
    if (!f.is_open()) {
        std::cerr << "[Error] Failed to open output file for run phase: " << out_path << "\n";
        return false;
    }

    std::cout << "  -> Starting run phase (" << wk_name << ") ... Target ops: " << target_count << "\n";

    std::stringstream cmd_ss;
    cmd_ss << "java -Xmx8g -cp \"" << classpath << "\" site.ycsb.Client -t " 
           << " -db site.ycsb.BasicDB -P " << wk_path.string() 
           << " -p recordcount=" << ycsb_record_count 
           << " -p operationcount=" << target_count 
           << " -s"; 

    std::string cmd = cmd_ss.str();

#ifdef _WIN32
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif

    if (!pipe) {
        return BenchmarkGeneration::finish_ycsb_output(
            pipe, f, out_path, 0, target_count);
    }
    char buffer[1024];
    uint64_t count = 0;

    std::mt19937_64 bucket_rand(1337); 

    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {

        std::istringstream iss(buffer);
        std::string op, table, key_str, extra;

        if (iss >> op >> table >> key_str && VALID_OPS.count(op)) {
            try {
                uint64_t raw_id = parse_ycsb_key(key_str);

                if (scale_factor > 1) {
                    std::uniform_int_distribution<uint64_t> offset_dis(0, scale_factor - 1);
                    uint64_t offset = offset_dis(bucket_rand);
                    raw_id = (raw_id * scale_factor) + offset;
                }

                uint64_t key = deterministic_hash_map(raw_id, domain_size);

                if (op == "SCAN") {
                    iss >> extra;
                    uint64_t len = std::stoull(extra);
                    // Cap scan length to the configured maximum.
                    if (len > max_scan_len_cap) len = max_scan_len_cap;
                    f << "SCAN " << key << " " << len << "\n";
                    count++;
                } else if (op == "READ") {
                    f << "GET " << key << "\n";
                    count++;
                }
            } catch (...) { continue; }
        }
    }

    if (!BenchmarkGeneration::finish_ycsb_output(
            pipe, f, out_path, count, target_count)) {
        return false;
    }
    std::cout << "  -> Completed " << count << " lines in " << out_path.filename() << "\n";
    return true;
}

bool ensure_directory(const fs::path& dir_path) {
    std::error_code ec;
    fs::create_directories(dir_path, ec);
    if (ec) {
        std::cerr << "[Path Error] Failed to create directory " << dir_path
                  << ": " << ec.message() << "\n";
        return false;
    }

    if (!fs::exists(dir_path) || !fs::is_directory(dir_path)) {
        std::cerr << "[Path Error] Directory verification failed: " << dir_path << "\n";
        return false;
    }
    return true;
}

bool verify_non_empty_file(const fs::path& file_path, const std::string& label) {
    std::error_code ec;
    if (!fs::exists(file_path, ec) || ec) {
        std::cerr << "[Path Error] Missing " << label << ": " << file_path << "\n";
        return false;
    }

    const uint64_t sz = fs::file_size(file_path, ec);
    if (ec || sz == 0) {
        std::cerr << "[Data Error] " << label << " is empty or unreadable: " << file_path << "\n";
        return false;
    }
    return true;
}

void remove_file_if_exists(const fs::path& file_path) {
    std::error_code ec;
    if (fs::exists(file_path, ec) && !ec) {
        fs::remove(file_path, ec);
        if (ec) {
            std::cerr << "[Warning] Failed to remove obsolete file " << file_path
                      << ": " << ec.message() << "\n";
        }
    }
}

bool copy_file_with_verification(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    if (!fs::exists(src, ec) || ec || !fs::is_regular_file(src)) {
        std::cerr << "[Path Error] Source file does not exist: " << src << "\n";
        return false;
    }

    if (!ensure_directory(dst.parent_path())) {
        return false;
    }

    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::cerr << "[Path Error] Failed to copy " << src << " to " << dst
                  << ": " << ec.message() << "\n";
        return false;
    }

    return verify_non_empty_file(dst, dst.filename().string());
}

bool generate_init_data(const fs::path& ycsb_home,
                        const fs::path& out_path,
                        uint64_t total_record_count,
                        uint64_t domain_size) {
    if (!ensure_directory(out_path.parent_path())) {
        return false;
    }

    std::ofstream clear_f(out_path, std::ios_base::out);
    if (!clear_f.is_open()) {
        std::cerr << "[Error] Failed to initialize output file: " << out_path << "\n";
        return false;
    }
    clear_f.close();

    if (total_record_count <= BATCH_SIZE_LIMIT) {
        std::cout << "  -> Generating InitData in one batch...\n";
        if (!run_ycsb_batch_load(ycsb_home, "wk_load_only", out_path,
                                 total_record_count, 0, domain_size)) {
            return false;
        }
    } else {
        const uint64_t full_batches = total_record_count / BATCH_SIZE_LIMIT;
        const uint64_t remainder = total_record_count % BATCH_SIZE_LIMIT;

        std::cout << "  -> Generating InitData in " << full_batches
                  << " full batches and " << (remainder > 0 ? 1 : 0)
                  << " remainder batch...\n";

        for (uint64_t i = 0; i < full_batches; ++i) {
            const uint64_t current_offset = i * BATCH_SIZE_LIMIT;
            if (!run_ycsb_batch_load(ycsb_home, "wk_load_only", out_path,
                                     BATCH_SIZE_LIMIT, current_offset, domain_size)) {
                return false;
            }
        }

        if (remainder > 0) {
            const uint64_t final_offset = full_batches * BATCH_SIZE_LIMIT;
            if (!run_ycsb_batch_load(ycsb_home, "wk_load_only", out_path,
                                     remainder, final_offset, domain_size)) {
                return false;
            }
        }
    }

    return verify_non_empty_file(out_path, "YCSBInitData.txt");
}

bool copy_exp2_init_and_get_only(const fs::path& src_dir, const fs::path& dst_dir) {
    if (!copy_file_with_verification(src_dir / "YCSBInitData.txt", dst_dir / "YCSBInitData.txt")) {
        return false;
    }
    if (!copy_file_with_verification(src_dir / "YCSBGet.txt", dst_dir / "YCSBGet.txt")) {
        return false;
    }
    remove_file_if_exists(dst_dir / "YCSBScan.txt");
    return true;
}

int main() {
    fs::path ycsb_home = fs::absolute(YCSB_HOME_REL);

    if (!fs::exists(ycsb_home) || !fs::is_directory(ycsb_home)) {
        std::cerr << "[Fatal Error] Invalid YCSB home path: " << ycsb_home << "\n";
        return 1;
    }

    // Base workload definitions used by all generated datasets.
    create_workload_file(ycsb_home / "workloads" / "wk_load_only",
        "readproportion=0\nupdateproportion=0\ninsertproportion=1\nscanproportion=0\n", true);
    create_workload_file(ycsb_home / "workloads" / "wk_read_only",
        "readproportion=1\nupdateproportion=0\ninsertproportion=0\nscanproportion=0\n", false);

    for (int power = BLOCKSIZE_MIN_POWER; power <= BLOCKSIZE_MAX_POWER; ++power) {
        uint64_t total_record_count = 1ULL << power;
        uint64_t domain_size = 100ULL * total_record_count;
        std::string dir_name = "YCSB_2^" + std::to_string(power);
        fs::path blocksize_dir = fs::path(EXP2_BLOCKSIZE_DIR) / dir_name;

        std::cout << "\n======================================================\n";
        std::cout << "Generating Exp2/blocksize dataset: " << blocksize_dir << "\n";
        std::cout << "Record Count: " << total_record_count << ", Target Ops: " << OPERATION_COUNT << "\n";
        std::cout << "======================================================\n";

        fs::path init_path = blocksize_dir / "YCSBInitData.txt";
        fs::path get_path = blocksize_dir / "YCSBGet.txt";

        if (!generate_init_data(ycsb_home, init_path, total_record_count, domain_size)) {
            return 1;
        }

        if (!run_ycsb_run_phase(ycsb_home, "wk_read_only", get_path,
                                total_record_count, OPERATION_COUNT, domain_size)) {
            return 1;
        }
        if (!verify_non_empty_file(get_path, "YCSBGet.txt")) {
            return 1;
        }

        // Exp2 blocksize/buffersize/fanout/privacy should not carry range traces.
        remove_file_if_exists(blocksize_dir / "YCSBScan.txt");

        if (power == SHARED_DATA_POWER) {
            std::cout << "  -> Syncing 2^24 InitData/Get to Exp2 parameter folders...\n";

            if (!copy_exp2_init_and_get_only(blocksize_dir,
                    fs::path(EXP2_BUFFERSIZE_DIR) / dir_name)) {
                return 1;
            }
            if (!copy_exp2_init_and_get_only(blocksize_dir,
                    fs::path(EXP2_FANOUT_DIR) / dir_name)) {
                return 1;
            }
            if (!copy_exp2_init_and_get_only(blocksize_dir,
                    fs::path(EXP2_PRIVACY_DIR) / dir_name)) {
                return 1;
            }
            if (!copy_exp2_init_and_get_only(blocksize_dir,
                    fs::path(EXP2_PAYLOAD_DIR) / dir_name)) {
                return 1;
            }

            // Exp1/rangequery keeps InitData and dedicated scan files only.
            fs::path range_query_dir = fs::path(EXP1_RANGEQUERY_DIR) / dir_name;
            if (!copy_file_with_verification(init_path, range_query_dir / "YCSBInitData.txt")) {
                return 1;
            }
            remove_file_if_exists(range_query_dir / "YCSBGet.txt");
            remove_file_if_exists(range_query_dir / "YCSBScan.txt");

            const uint64_t lengths[] = {
                100ULL, 1000ULL, 10000ULL, 100000ULL,
                1000000ULL, 10000000ULL, 100000000ULL
            };
            const char* length_names[] = {"1e2", "1e3", "1e4", "1e5", "1e6", "1e7", "1e8"};

            for (int i = 0; i < 7; ++i) {
                const uint64_t max_scan_len = lengths[i];
                const std::string current_wk_name = "wk_scan_range_" + std::string(length_names[i]);

                std::cout << "  -> Generating Exp1/rangequery scan trace with maxscanlength="
                          << length_names[i] << "\n";

                create_workload_file(ycsb_home / "workloads" / current_wk_name,
                    "readproportion=0\nupdateproportion=0\ninsertproportion=0\nscanproportion=1\n",
                    false,
                    max_scan_len);

                fs::path scan_out = range_query_dir / ("YCSBScan_" + std::string(length_names[i]) + ".txt");
                if (!run_ycsb_run_phase(ycsb_home, current_wk_name, scan_out,
                                        total_record_count, OPERATION_COUNT,
                                        domain_size, max_scan_len)) {
                    return 1;
                }
                if (!verify_non_empty_file(scan_out, "rangequery scan trace")) {
                    return 1;
                }
            }
        }
    }

    std::cout << "\nAll required Exp2 and Exp1 trace artifacts were generated successfully.\n";
    return 0;
}