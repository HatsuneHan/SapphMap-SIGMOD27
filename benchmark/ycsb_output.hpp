#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace BenchmarkGeneration {

// Consume stdout to EOF before calling this, so closing the pipe does not
// interrupt YCSB while it is still writing its final output.
inline bool finish_ycsb_output(FILE* pipe, std::ofstream& output,
                               const std::filesystem::path& path,
                               uint64_t count, uint64_t expected) {
    const bool read_failed = pipe == nullptr || std::ferror(pipe) != 0;
    int status = -1;
    if (pipe != nullptr) {
#ifdef _WIN32
        status = _pclose(pipe);
#else
        status = pclose(pipe);
#endif
    }
    output.close();
    if (!read_failed && status == 0 && output && count == expected) {
        return true;
    }
    std::cerr << "[Data Error] YCSB generation failed for " << path
              << ": child status=" << status << ", expected=" << expected
              << ", generated=" << count << ", read_failed=" << read_failed
              << ", write_failed=" << !output << "\n";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) {
        std::cerr << "[Data Error] Cannot remove incomplete output: "
                  << ec.message() << "\n";
    }
    return false;
}

}  // namespace BenchmarkGeneration
