#include "App.h"

#include <assert.h>
#include <limits.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "Enclave_u.h"
#include "sgx_urts.h"

/* Global EID shared by multiple threads */
sgx_enclave_id_t global_eid = 0;

typedef struct _sgx_errlist_t {
  sgx_status_t err;
  const char *msg;
  const char *sug; /* Suggestion */
} sgx_errlist_t;

/* Error code returned by sgx_create_enclave */
static sgx_errlist_t sgx_errlist[] = {
    {SGX_ERROR_UNEXPECTED, "Unexpected error occurred.", NULL},
    {SGX_ERROR_INVALID_PARAMETER, "Invalid parameter.", NULL},
    {SGX_ERROR_OUT_OF_MEMORY, "Out of memory.", NULL},
    {SGX_ERROR_ENCLAVE_LOST, "Power transition occurred.",
     "Please refer to the sample \"PowerTransition\" for details."},
    {SGX_ERROR_INVALID_ENCLAVE, "Invalid enclave image.", NULL},
    {SGX_ERROR_INVALID_ENCLAVE_ID, "Invalid enclave identification.", NULL},
    {SGX_ERROR_INVALID_SIGNATURE, "Invalid enclave signature.", NULL},
    {SGX_ERROR_OUT_OF_EPC, "Out of EPC memory.", NULL},
    {SGX_ERROR_NO_DEVICE, "Invalid SGX device.",
     "Please make sure SGX module is enabled in the BIOS, and install SGX "
     "driver afterwards."},
    {SGX_ERROR_MEMORY_MAP_CONFLICT, "Memory map conflicted.", NULL},
    {SGX_ERROR_INVALID_METADATA, "Invalid enclave metadata.", NULL},
    {SGX_ERROR_DEVICE_BUSY, "SGX device was busy.", NULL},
    {SGX_ERROR_INVALID_VERSION, "Enclave version was invalid.", NULL},
    {SGX_ERROR_INVALID_ATTRIBUTE, "Enclave was not authorized.", NULL},
    {SGX_ERROR_ENCLAVE_FILE_ACCESS, "Can't open enclave file.", NULL},
    {SGX_ERROR_NDEBUG_ENCLAVE,
     "The enclave is signed as product enclave, and can not be created as "
     "debuggable enclave.",
     NULL},
};

/* Check error conditions for loading enclave */
void print_error_message(sgx_status_t ret) {
  size_t idx = 0;
  size_t ttl = sizeof sgx_errlist / sizeof sgx_errlist[0];

  for (idx = 0; idx < ttl; idx++) {
    if (ret == sgx_errlist[idx].err) {
      if (NULL != sgx_errlist[idx].sug)
        printf("Info: %s\n", sgx_errlist[idx].sug);
      printf("Error: %s\n", sgx_errlist[idx].msg);
      break;
    }
  }

  if (idx == ttl) printf("Error: Unexpected error occurred.\n");
}

/* Initialize the enclave:
 *   Call sgx_create_enclave to initialize an enclave instance
 */
int initialize_enclave(void) {
  sgx_status_t ret = SGX_ERROR_UNEXPECTED;

  /* Call sgx_create_enclave to initialize an enclave instance */
  /* Debug Support: set 2nd parameter to 1 */
  ret = sgx_create_enclave(ENCLAVE_FILENAME, SGX_DEBUG_FLAG, NULL, NULL,
                           &global_eid, NULL);
  if (ret != SGX_SUCCESS) {
    print_error_message(ret);
    return -1;
  }

  return 0;
}

/* OCall functions */
void ocall_print_string(const char *str) {
  /* Proxy/Bridge will check the length and null-terminate
   * the input string to prevent buffer overflow.
   */
  printf("%s", str);
}

void ocall_FAIL() {
  printf("Called FAIL oCall\n");
  exit(-1);
}

#include <chrono>

uint64_t ocall_measure_time(void) {
  // returns linux-epoch-time in nanoseconds.
  auto now_c = std::chrono::system_clock::now();
  uint64_t ret = now_c.time_since_epoch().count();
  return ret;
}

/* Result-file OCalls for test_kv_stores_correctness */
static FILE* g_result_fp = nullptr;

void ocall_result_file_open(const char* path) {
  if (g_result_fp) fclose(g_result_fp);
  g_result_fp = fopen(path, "wb");
}

void ocall_result_file_write(const uint8_t* buf, uint64_t len) {
  if (g_result_fp) fwrite(buf, 1, (size_t)len, g_result_fp);
}

void ocall_result_file_close(void) {
  if (g_result_fp) { fclose(g_result_fp); g_result_fp = nullptr; }
}

int ocall_result_files_equal(const char* f1, const char* f2,
                             const char* f3, const char* f4) {
  const char* files[4] = {f1, f2, f3, f4};
  FILE* fp0 = fopen(files[0], "rb");
  if (!fp0) return 0;
  fseek(fp0, 0, SEEK_END);
  long sz0 = ftell(fp0);
  rewind(fp0);
  unsigned char* buf0 = new unsigned char[(size_t)sz0];
  if (fread(buf0, 1, (size_t)sz0, fp0) != (size_t)sz0) {
    fclose(fp0); delete[] buf0; return 0;
  }
  fclose(fp0);

  int all_eq = 1;
  for (int i = 1; i < 4 && all_eq; ++i) {
    FILE* fp = fopen(files[i], "rb");
    if (!fp) { all_eq = 0; break; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    rewind(fp);
    if (sz != sz0) { fclose(fp); all_eq = 0; break; }
    unsigned char* buf = new unsigned char[(size_t)sz];
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz ||
        memcmp(buf0, buf, (size_t)sz0) != 0)
      all_eq = 0;
    fclose(fp);
    delete[] buf;
  }
  delete[] buf0;
  return all_eq;
}

/* Sequential file reading OCalls for experiment workloads */
static FILE* g_exp_read_fp = nullptr;

void ocall_exp_file_open(const char* path) {
  if (g_exp_read_fp) fclose(g_exp_read_fp);
  g_exp_read_fp = fopen(path, "r");
}

int64_t ocall_exp_file_read_line(char* buf, uint64_t buf_size) {
  if (!g_exp_read_fp) return -1;
  if (fgets(buf, (int)buf_size, g_exp_read_fp) == nullptr) return 0;
  return (int64_t)strlen(buf);
}

void ocall_exp_file_close(void) {
  if (g_exp_read_fp) { fclose(g_exp_read_fp); g_exp_read_fp = nullptr; }
}

int64_t ocall_exp_file_read_bulk(uint64_t line_count, uint64_t max_len, char* buf) {
  if (!g_exp_read_fp || buf == nullptr || max_len == 0 || line_count == 0)
    return -1;
  uint64_t off = 0;
  for (uint64_t i = 0; i < line_count; ++i) {
    if (off >= max_len) return -1;
    uint64_t room64 = max_len - off;
    int fgets_n = (room64 > (uint64_t)INT_MAX) ? INT_MAX : (int)room64;
    if (fgets_n < 2) return -1;
    char* dst = buf + off;
    if (fgets(dst, fgets_n, g_exp_read_fp) == nullptr) {
      if (ferror(g_exp_read_fp)) return -1;
      return (off == 0) ? 0 : (int64_t)off;
    }
    size_t len = strlen(dst);
    if (len == 0) return -1;
    if (dst[len - 1] != '\n') {
      if (!feof(g_exp_read_fp)) return -1;
      if (off + len + 1 > max_len) return -1;
      buf[off + len] = '\n';
      off += len + 1;
      break;
    }
    off += len;
  }
  return (int64_t)off;
}

/* Application entry */
int SGX_CDECL main(int argc, char *argv[]) {
  (void)(argc);
  (void)(argv);

  /* Initialize the enclave */
  if (initialize_enclave() < 0) {
    printf("Enter a character before exit ...\n");
    getchar();
    return -1;
  }

  /* Utilize trusted libraries */
  ActualMain();

  /* Destroy the enclave */
  sgx_destroy_enclave(global_eid);

  return 0;
}
