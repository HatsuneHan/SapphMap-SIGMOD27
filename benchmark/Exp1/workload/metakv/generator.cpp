#include <iostream>
#include <fstream>
#include <string>
#include <random>
#include <cstdint>
#include <stdexcept>

constexpr uint64_t kTraceLineLimit = 1ULL << 27;

bool writeTruncatedTrace(const std::string& full_path,
                        const std::string& truncated_path,
                        uint64_t max_lines) {
    std::ifstream full_in(full_path);
    if (!full_in.is_open()) {
        std::cerr << "Error: Could not open full trace file " << full_path << std::endl;
        return false;
    }

    std::ofstream truncated_out(truncated_path);
    if (!truncated_out.is_open()) {
        std::cerr << "Error: Could not create truncated trace file " << truncated_path << std::endl;
        return false;
    }

    std::string line;
    uint64_t lines_written = 0;
    while (lines_written < max_lines && std::getline(full_in, line)) {
        truncated_out << line << "\n";
        ++lines_written;
    }

    std::cout << "Truncated trace saved to " << truncated_path
              << " (" << lines_written << " lines)." << std::endl;
    if (lines_written < max_lines) {
        std::cerr << "Warning: generated trace is shorter than expected (target "
                  << max_lines << ", got " << lines_written
                  << "). Exp1/workload replay expects 2^27 lines." << std::endl;
    }
    return true;
}

int main() {
    const std::string input_filename = "./raw/202401_kv_traces_all_sort.csv";
    const std::string full_output_filename = "MetaKVFullTrace.txt";
    const std::string truncated_output_filename = "MetaKVTrace.txt";

    std::ifstream infile(input_filename);
    if (!infile.is_open()) {
        std::cerr << "Error: Could not open input file " << input_filename << std::endl;
        return 1;
    }

    std::ofstream outfile(full_output_filename);
    if (!outfile.is_open()) {
        std::cerr << "Error: Could not open output file " << full_output_filename << std::endl;
        return 1;
    }

    // Set up 64-bit random number generator for PUT values
    std::random_device rd;
    std::mt19937_64 rng(rd());
    std::uniform_int_distribution<uint64_t> dist;

    std::string line;
    uint64_t line_number = 0;

    while (std::getline(infile, line)) {
        line_number++;
        if (line.empty()) continue;

        size_t pos = 0, next_pos = 0;

        // 0. Skip op_time
        next_pos = line.find(',', pos);
        if (next_pos == std::string::npos) continue;
        pos = next_pos + 1;

        // 1. Extract key
        next_pos = line.find(',', pos);
        if (next_pos == std::string::npos) continue;
        std::string key_str = line.substr(pos, next_pos - pos);
        pos = next_pos + 1;

        // 2. Skip key_size
        next_pos = line.find(',', pos);
        if (next_pos == std::string::npos) continue;
        pos = next_pos + 1;

        // 3. Extract op
        next_pos = line.find(',', pos);
        if (next_pos == std::string::npos) continue;
        std::string op_str = line.substr(pos, next_pos - pos);
        pos = next_pos + 1;

        // 4. Extract op_count
        next_pos = line.find(',', pos);
        std::string op_count_str = (next_pos == std::string::npos) ? 
                                   line.substr(pos) : 
                                   line.substr(pos, next_pos - pos);

        try {
            // Convert Hex string to uint64_t
            uint64_t key = std::stoull(key_str, nullptr, 16);
            
            // Convert op_count
            int op_count = std::stoi(op_count_str);

            // Map operations
            std::string mapped_op;
            if (op_str == "GET" || op_str == "GET_LEASE") {
                mapped_op = "GET";
            } else if (op_str == "SET") {
                mapped_op = "PUT";
            } else if (op_str == "DELETE") {
                mapped_op = "DELETE";
            } else {
                continue; // Skip unknown operations
            }

            // Write output repeated op_count times
            for (int i = 0; i < op_count; ++i) {
                if (mapped_op == "PUT") {
                    outfile << mapped_op << " " << key << " " << dist(rng) << "\n";
                } else {
                    outfile << mapped_op << " " << key << "\n";
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Warning: Parsing error on line " << line_number << " (" << e.what() << ")\n";
        }
    }

    infile.close();
    outfile.close();

    if (!writeTruncatedTrace(full_output_filename, truncated_output_filename, kTraceLineLimit)) {
        return 1;
    }
    
    std::cout << "Successfully converted traces to " << full_output_filename << std::endl;
    return 0;
}