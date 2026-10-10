#include <stdio.h>

#include <thread>

#include "../App.h"
#include "Enclave_u.h"
#ifdef DISK_IO
// #include "external_memory/server/enclaveFileServer_untrusted.hpp"  // full disk I/O (fstream)
#include "external_memory/server/enclaveMmapFileServer_untrusted.hpp"  // hybrid RAM+disk (mmap)
#else
#include "external_memory/server/enclaveMemServer_untrusted.hpp"
#endif

void ActualMain(void) {
  sgx_status_t ret = SGX_ERROR_UNEXPECTED;

  ret = ecall_sapphmap_test(global_eid);

  if (ret != SGX_SUCCESS) abort();

  // printf("Did not abort\n");
}
