#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <sstream>
#include <filesystem>
#include <cstdio>
#include <cstdint>
#include <algorithm>
#include <random>

#include "../../ycsb_output.hpp"

namespace fs = std::filesystem;

// Core configuration.
const std::string OUTPUT_DIR = "./";
// This relative path is intended when running from benchmark/Exp1/scalability.
const std::string YCSB_HOME_REL = "../../dependency/ycsb-0.17.0";

// Conservative Java-side integer safety limit: 2^30.
const uint64_t BATCH_SIZE_LIMIT = 1ULL << 30;

std::string get_ycsb_classpath(const fs::path& ycsb_home) {
    if (!fs::exists(ycsb_home)) {
        std::cerr << "\n[Fatal Error] YCSB directory does not exist: " << ycsb_home << "\n";
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

// Stable key mapping; domain_size is fixed at 100x the real record count.
uint64_t deterministic_hash_map(uint64_t raw_id, uint64_t domain_size) {
    return (raw_id * 11400714819323198483ULL % domain_size) + 1;
}

uint64_t generate_value(uint64_t raw_id) {
    return (raw_id * 6364136223846793005ULL + 1442695040888963407ULL) & 0xFFFFFFFFFFFFFFFFULL;
}

// ============================================================================
// 1. Generate workload configs.
//    Use ordered inserts during load so C++ controls final key hashing.
// ============================================================================
void create_workload_file(const fs::path& wk_path, const std::string& proportions, bool is_load_phase) {
    fs::create_directories(wk_path.parent_path());
    std::ofstream f(wk_path);
    if (!f.is_open()) exit(1);
    
    // Load must stay ordered so batched concatenation has no key gaps.
    std::string insert_order = is_load_phase ? "ordered" : "hashed";

    f << "workload=site.ycsb.workloads.CoreWorkload\n"
      << "requestdistribution=zipfian\n"
      << "zipfianconstant=0.99\n"
      << "insertorder=" << insert_order << "\n" 
      << "fieldcount=1\n"
      << "fieldlength=8\n"
      << proportions;
    f.close();
}

// ============================================================================
// 2. Load phase: append batch output with base_offset and required Java params.
// ============================================================================
bool run_ycsb_batch_load(const fs::path& ycsb_home, const std::string& wk_name,
                         const fs::path& out_path, uint64_t local_record_count, uint64_t base_offset, uint64_t domain_size) {
    
    std::string classpath = get_ycsb_classpath(ycsb_home);
    fs::path wk_path = ycsb_home / "workloads" / wk_name;
    
    std::ofstream f(out_path, std::ios_base::app);
    if (!f.is_open()) {
        std::cerr << "[Error] Cannot open output file: " << out_path << "\n";
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

// ============================================================================
// 3. Run phase: large datasets automatically use bucket-style expansion.
// ============================================================================
bool run_ycsb_run_phase(const fs::path& ycsb_home, const std::string& wk_name,
                        const fs::path& out_path, uint64_t total_record_count, uint64_t target_count, uint64_t domain_size) {
    
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
        std::cerr << "[Error] Cannot open output file: " << out_path << "\n";
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
        std::string op, table, key_str;

        if (iss >> op >> table >> key_str && op == "READ") {
            try {
                uint64_t raw_id = parse_ycsb_key(key_str);

                if (scale_factor > 1) {
                    std::uniform_int_distribution<uint64_t> offset_dis(0, scale_factor - 1);
                    uint64_t offset = offset_dis(bucket_rand);
                    raw_id = (raw_id * scale_factor) + offset;
                }

                uint64_t key = deterministic_hash_map(raw_id, domain_size);

                f << "GET " << key << "\n";
                count++;
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

int main() {
    fs::path ycsb_home = fs::absolute(YCSB_HOME_REL);

    create_workload_file(ycsb_home / "workloads" / "wk_load_only",
        "readproportion=0\nupdateproportion=0\ninsertproportion=1\nscanproportion=0\n", true);
    create_workload_file(ycsb_home / "workloads" / "wk_read_only",
        "readproportion=1\nupdateproportion=0\ninsertproportion=0\nscanproportion=0\n", false);

    for (int power = 18; power <= 31; ++power) {
        uint64_t total_record_count = 1ULL << power;
        uint64_t operation_count = 65536; 
        uint64_t domain_size = 100ULL * total_record_count;

        std::string dir_name = "YCSB_2^" + std::to_string(power);
        fs::path current_out_dir = fs::path(OUTPUT_DIR) / dir_name;
        fs::create_directories(current_out_dir);

        std::cout << "\n======================================================\n";
        std::cout << "Generating data for directory: " << dir_name << "\n";
        std::cout << "Record Count: " << total_record_count << ", Domain: " << domain_size << "\n";
        std::cout << "======================================================\n";

        fs::path final_load_path = current_out_dir / "YCSBInitData.txt";

        std::ofstream clear_f(final_load_path, std::ios_base::out);
        clear_f.close();
        if (!clear_f) {
            std::cerr << "[Error] Cannot initialize output: " << final_load_path << "\n";
            return 1;
        }

        uint64_t k_batches = total_record_count / BATCH_SIZE_LIMIT;
        uint64_t m_remainder = total_record_count % BATCH_SIZE_LIMIT;

        if (total_record_count <= BATCH_SIZE_LIMIT) {
            std::cout << "  -> Small scale data, executing standard load...\n";
            if (!run_ycsb_batch_load(ycsb_home, "wk_load_only", final_load_path, total_record_count, 0, domain_size)) return 1;
        } else {
            std::cout << "  -> Massive scale data, splitting into " << k_batches << " full batches and "
                      << (m_remainder > 0 ? 1 : 0) << " remainder batch...\n";

            for (uint64_t i = 0; i < k_batches; ++i) {
                uint64_t current_offset = i * BATCH_SIZE_LIMIT;
                std::cout << "    -> Running batch " << i << " (Offset: " << current_offset << ")\n";
                if (!run_ycsb_batch_load(ycsb_home, "wk_load_only", final_load_path, BATCH_SIZE_LIMIT, current_offset, domain_size)) return 1;
            }

            if (m_remainder > 0) {
                uint64_t final_offset = k_batches * BATCH_SIZE_LIMIT;
                std::cout << "    -> Running remainder batch (Offset: " << final_offset << ", Size: " << m_remainder << ")\n";
                if (!run_ycsb_batch_load(ycsb_home, "wk_load_only", final_load_path, m_remainder, final_offset, domain_size)) return 1;
            }
        }
        std::cout << "  -> Final InitData file completed: " << final_load_path.filename() << "\n";

        // Use the configured per-trace operation count.
        if (!run_ycsb_run_phase(ycsb_home, "wk_read_only", current_out_dir / "YCSBGet.txt", total_record_count, operation_count, domain_size)) return 1;
    }

    std::cout << "\nAll trace generation finished successfully.\n";
    return 0;
}
