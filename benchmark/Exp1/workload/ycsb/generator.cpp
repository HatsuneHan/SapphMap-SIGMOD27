#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <sstream>
#include <filesystem>
#include <cstdio>
#include <cstdint>
#include <algorithm>
#include <unordered_set>
#include <random>

#include "../../../ycsb_output.hpp"

namespace fs = std::filesystem;

// Configuration
const std::string OUTPUT_DIR = "./";
const std::string YCSB_HOME_REL = "../../../dependency/ycsb-0.17.0";

const uint64_t RECORD_COUNT = 1ULL << 26;    // 67,108,864
const uint64_t OPERATION_COUNT = 1ULL << 26; // 67,108,864

const uint64_t MAX_WRITES_N = RECORD_COUNT + OPERATION_COUNT;
const uint64_t DOMAIN_SIZE = 100 * MAX_WRITES_N;

// YCSB emits these operation tags in stdout traces.
const std::unordered_set<std::string> VALID_OPS = {"INSERT", "READ", "UPDATE", "SCAN"};

std::string get_ycsb_classpath(const fs::path& ycsb_home) {
    std::vector<std::string> cp = {
        (ycsb_home / "conf").string(),
        (ycsb_home / "lib" / "*").string(),
        (ycsb_home / "basic-binding" / "lib" / "*").string()
    };
    
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

// Restored deterministic hash map
uint64_t deterministic_hash_map(uint64_t raw_id) {
    return (raw_id * 11400714819323198483ULL % DOMAIN_SIZE) + 1;
}

uint64_t generate_value(uint64_t raw_id) {
    return (raw_id * 6364136223846793005ULL + 1442695040888963407ULL) & 0xFFFFFFFFFFFFFFFFULL;
}

void create_workload_file(const fs::path& wk_path, const std::string& proportions, bool is_load_phase = false) {
    fs::create_directories(wk_path.parent_path());
    std::ofstream f(wk_path);
    if (!f.is_open()) {
        std::cerr << "Error: Cannot create workload config file " << wk_path << "\n";
        exit(1);
    }

    const std::string insert_order = is_load_phase ? "ordered" : "hashed";

    f << "workload=site.ycsb.workloads.CoreWorkload\n"
      << "requestdistribution=zipfian\n"
      << "zipfianconstant=0.99\n"
      << "maxscanlength=1000\n"
      << "scanlengthdistribution=uniform\n"
      << "insertorder=" << insert_order << "\n"
      << "fieldcount=1\n"
      << "fieldlength=8\n"
      << proportions;
    f.close();
}

bool run_ycsb_phase(const fs::path& ycsb_home, const std::string& phase, const std::string& wk_name, const std::string& out_filename) {
    if (!fs::exists(OUTPUT_DIR)) fs::create_directories(OUTPUT_DIR);

    std::string classpath = get_ycsb_classpath(ycsb_home);
    fs::path wk_path = ycsb_home / "workloads" / wk_name;
    fs::path out_path = fs::path(OUTPUT_DIR) / out_filename;
    
    std::ofstream f(out_path);
    if (!f.is_open()) {
        std::cerr << "Error: Cannot open output file " << out_path << "\n";
        return false;
    }

    std::cout << "\nStarting " << phase << " phase for " << wk_name << "... Output: " << out_filename << "\n";

    std::string phase_arg = (phase == "load") ? "-load" : "-t";
    std::stringstream cmd_ss;
    cmd_ss << "java -Xmx8g -cp \"" << classpath << "\" site.ycsb.Client " << phase_arg 
           << " -db site.ycsb.BasicDB -P " << wk_path.string() 
           << " -p recordcount=" << RECORD_COUNT;

    if (phase == "load") {
        cmd_ss << " -p insertstart=0"
               << " -p insertcount=" << RECORD_COUNT
               << " -p operationcount=0";
    } else {
        cmd_ss << " -p operationcount=" << OPERATION_COUNT;
    }

    cmd_ss << " -s";

    std::string cmd = cmd_ss.str();

#ifdef _WIN32
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif

    const uint64_t target_count = (phase == "load") ? RECORD_COUNT : OPERATION_COUNT;
    if (!pipe) {
        return BenchmarkGeneration::finish_ycsb_output(
            pipe, f, out_path, 0, target_count);
    }

    char buffer[1024];
    uint64_t count = 0;

    // Fixed seed keeps UPDATE->DELETE split stable across runs.
    std::mt19937 gen(1337); 
    std::uniform_real_distribution<> dis(0.0, 1.0);

    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {

        std::istringstream iss(buffer);
        std::string op, table, key_str, extra;

        if (iss >> op >> table >> key_str && VALID_OPS.count(op)) {
            try {
                // Apply the mapping to scatter keys in the domain space
                uint64_t raw_id = parse_ycsb_key(key_str);
                uint64_t key = deterministic_hash_map(raw_id);

                if (op == "SCAN") {
                    iss >> extra;
                    f << "SCAN " << key << " " << extra << "\n";
                } else if (op == "INSERT") {
                    f << "PUT " << key << " " << generate_value(raw_id) << "\n";
                } else if (op == "UPDATE") {
                    // For wk_mix run phase, split UPDATE into DELETE/PUT at 50/50.
                    if (wk_name == "wk_mix" && phase == "run" && dis(gen) < 0.5) {
                        f << "DELETE " << key << "\n";
                    } else {
                        f << "PUT " << key << " " << generate_value(raw_id) << "\n";
                    }
                } else if (op == "READ") {
                    f << "GET " << key << "\n";
                }

                if (++count % 1000000 == 0) {
                    std::cout << "        Written " << (count / 1000000) << "M lines\r" << std::flush;
                }
            } catch (...) { continue; }
        }
    }

    if (!BenchmarkGeneration::finish_ycsb_output(
            pipe, f, out_path, count, target_count)) {
        return false;
    }
    std::cout << "\nCompleted " << count << " lines.\n";
    return true;
}

int main() {
    fs::path ycsb_home = fs::absolute(YCSB_HOME_REL);
    if (!fs::exists(ycsb_home)) {
        std::cerr << "Error: YCSB home does not exist: " << ycsb_home << "\n";
        return 1;
    }

    // 1. Create pure-insert load workload for InitData generation.
    create_workload_file(ycsb_home / "workloads" / "wk_load_only",
        "readproportion=0\nupdateproportion=0\ninsertproportion=1\nscanproportion=0\n", true);

    // 2. Create mixed workload config.
    create_workload_file(ycsb_home / "workloads" / "wk_mix",
        "readproportion=0.35\nupdateproportion=0.4\ninsertproportion=0.2\nscanproportion=0.05\n", false);

    // 3. Create YCSB-A workload config (50% Read, 50% Update).
    create_workload_file(ycsb_home / "workloads" / "wk_a",
        "readproportion=0.5\nupdateproportion=0.5\ninsertproportion=0\nscanproportion=0\n", false);

    // Phase 1: InitData generation (2^26 pure INSERT -> PUT lines).
    if (!run_ycsb_phase(ycsb_home, "load", "wk_load_only", "YCSBInitData.txt")) return 1;

    // Phase 2: Mixed trace generation (2^26 operations).
    if (!run_ycsb_phase(ycsb_home, "run", "wk_mix", "YCSBMixTrace.txt")) return 1;

    // Phase 3: YCSB-A trace generation (2^26 operations).
    if (!run_ycsb_phase(ycsb_home, "run", "wk_a", "YCSBATrace.txt")) return 1;

    std::cout << "\nTrace generation finished successfully. Check " << OUTPUT_DIR << "\n";
    return 0;
}
