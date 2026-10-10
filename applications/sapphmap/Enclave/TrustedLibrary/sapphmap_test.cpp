#include "../Enclave.h"
#include "external_memory/test/experiment.hpp"

// experiment.hpp references this backend singleton in multiple code paths.
// Define it here so the enclave links without pulling in primitive_test.hpp.
namespace EM::Backend {
MemServerBackend* g_DefaultBackend = nullptr;
}

void ecall_sapphmap_test() {
    Experiment::run_bench_target();
}