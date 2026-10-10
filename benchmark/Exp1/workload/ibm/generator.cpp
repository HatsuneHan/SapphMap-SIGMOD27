#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <queue>
#include <filesystem>
#include <random>
#include <algorithm>
#include <sstream>

namespace fs = std::filesystem;
constexpr uint64_t kTraceLineLimit = 1ULL << 27;
const std::string kFullOutputFilename = "IBMObjectStoreFullTrace.txt";
const std::string kTruncatedOutputFilename = "IBMObjectStoreTrace.txt";

// Supported operation categories in the source trace.
enum class OpType { GET, PUT, DEL, UNKNOWN };

// One parsed record used by the k-way merge heap.
struct TraceRecord {
    uint64_t timestamp;  // Required for ordering across input shards.
    OpType op;
    uint64_t key;
    size_t file_index;   // Index of the input stream this record came from.

    // Reverse comparator for min-heap behavior in priority_queue.
    bool operator>(const TraceRecord& other) const {
        return timestamp > other.timestamp;
    }
};

// Tolerant parser: consume one line and use only the first valid fields.
bool parseNextLine(std::ifstream& stream, TraceRecord& record, size_t file_idx) {
    std::string line;
    
    while (std::getline(stream, line)) {
        if (line.empty()) continue;

        std::istringstream iss(line);
        std::string op_str;

        // Parse timestamp, operation token, and hexadecimal key only.
        if (iss >> record.timestamp >> op_str >> std::hex >> record.key) {
            
            if (op_str == "REST.GET.OBJECT" || op_str == "REST.HEAD.OBJECT") {
                record.op = OpType::GET;
            } else if (op_str == "REST.PUT.OBJECT") {
                record.op = OpType::PUT;
            } else if (op_str == "REST.DELETE.OBJECT") {
                record.op = OpType::DEL;
            } else {
                // Skip unrelated operations and continue parsing.
                continue; 
            }
            
            record.file_index = file_idx;
            return true;
        }
    }
    
    return false;  // Current file has no more valid lines.
}

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
    // Improve iostream throughput for large trace processing.
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(NULL);

    std::string raw_dir = "./raw";
    if (!fs::exists(raw_dir) || !fs::is_directory(raw_dir)) {
        std::cerr << "Error: Directory '" << raw_dir << "' does not exist." << std::endl;
        return 1;
    }

    std::vector<std::string> file_paths;
    
    // 1. Collect all matching trace shards under raw/.
    for (const auto& entry : fs::directory_iterator(raw_dir)) {
        if (entry.is_regular_file()) {
            std::string filename = entry.path().filename().string();
            if (filename.find("IBMObjectStoreTrace") == 0) {
                file_paths.push_back(entry.path().string());
            }
        }
    }

    if (file_paths.empty()) {
        std::cerr << "No trace files found in '" << raw_dir << "'." << std::endl;
        return 0;
    }

    // 2. Sort shards lexicographically by filename.
    std::sort(file_paths.begin(), file_paths.end());

    // 3. Open all input streams.
    std::vector<std::ifstream> streams;
    streams.reserve(file_paths.size());
    for (const auto& path : file_paths) {
        streams.emplace_back(path);
        if (!streams.back().is_open()) {
            std::cerr << "Warning: Failed to open " << path << std::endl;
        }
    }

    // 4. Initialize min-heap for k-way merge.
    std::priority_queue<TraceRecord, std::vector<TraceRecord>, std::greater<TraceRecord>> min_heap;

    // Seed heap with the first valid record from each stream.
    for (size_t i = 0; i < streams.size(); ++i) {
        TraceRecord rec;
        if (parseNextLine(streams[i], rec, i)) {
            min_heap.push(rec);
        }
    }

    // 5. Prepare full-output file and random generator for PUT values.
    std::ofstream out_file(kFullOutputFilename);
    if (!out_file.is_open()) {
        std::cerr << "Error: Could not create output file." << std::endl;
        return 1;
    }
    
    // 64-bit Mersenne Twister for synthetic PUT values.
    std::mt19937_64 rng(std::random_device{}());

    // 6. Core k-way merge loop.
    std::cout << "Starting merge process. Merging " << streams.size() << " files..." << std::endl;
    while (!min_heap.empty()) {
        TraceRecord current = min_heap.top();
        min_heap.pop();

        // Write normalized output without source timestamps.
        if (current.op == OpType::GET) {
            out_file << "GET " << current.key << "\n";
        } else if (current.op == OpType::PUT) {
            out_file << "PUT " << current.key << " " << rng() << "\n";
        } else if (current.op == OpType::DEL) {
            out_file << "DELETE " << current.key << "\n";
        }

        // Pull the next valid record from the same source stream.
        TraceRecord next_rec;
        if (parseNextLine(streams[current.file_index], next_rec, current.file_index)) {
            min_heap.push(next_rec);
        }
    }

    out_file.close();

    if (!writeTruncatedTrace(kFullOutputFilename, kTruncatedOutputFilename, kTraceLineLimit)) {
        return 1;
    }

    std::cout << "Merge completed successfully. Full output saved to "
              << kFullOutputFilename << std::endl;
    return 0;
}