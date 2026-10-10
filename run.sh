#!/bin/bash
# ============================================================
# run.sh - Unified Script to Build, Compile and Run SapphMap in Docker
#
# Usage:
#   ./run.sh [all]                          # (Default) Clean, compile, and run
#   ./run.sh build                          # Build Docker image
#   ./run.sh make [MAKE_ARGS...]            # Compile inside container (without run)
#   ./run.sh run  [CMD...]                  # Run sapphmap.elf (without compile)
#   ./run.sh shell                          # Interactive shell
#   ./run.sh clean                          # make clean
#   ./run.sh exec [CMD...]                  # Run arbitrary command
#
# Environment variables (all optional, with defaults):
#   RAM_SIZE       - Docker memory limit     (default: 128G)
#   ENCLAVE_SIZE   - Enclave heap size       (default: 32GB)
#   BLOCK_SIZE     - Cache page / block size (default: 32KB)
#   BUFFER_SIZE    - Buffer size             (default: 512KB)
#   GAMMA          - Level size multiplier   (default: 2)
#   EPS_TOTAL      - DP epsilon budget       (default: 4.0)
#   SGX_MODE       - SIM or HW               (default: HW)
#   SGX_PRERELEASE - 0 or 1                  (default: 1, highly recommended for HW)
#   SGX_DEBUG      - 0 or 1                  (default: 0)
#   DISK_IO        - 0 or 1                  (default: 1)
#   DISK_IO_DATA_DIR - host dir for storage.bin when DISK_IO=1
#                     (default: ${PROJECT_DIR}/.diskio)
#   DISK_IO_STORAGE_FILE - full storage.bin path passed to make
#                     (default: ${DISK_IO_DATA_DIR}/storage.bin)
#   IO_ROUND       - I/O round               (default: 1)
#   JOBS           - make parallelism        (default: $(nproc))
#   QUIET_MAKE     - 1 quiet / 0 verbose     (default: 1)
#   QUIET_DOCKER   - 1 hide docker noise      (default: 1)
#   QUIET_SUMMARY  - 1 hide run summary       (default: 1)
#   BENCH_DATASET_POW - dataset exponent      (default: 23)
#
#   Unified benchmark controls:
#   BENCH_TARGET   - exp1_scalability | exp1_workload | exp1_rangequery |
#                    exp2_privacy | exp2_fanout | exp2_buffersize |
#                    exp2_blocksize | exp2_payload | exp2_tradeoff
#   BENCH_INDEX    - sapphmap | nolsmtree | enigmap | nohashmap | h2o2ram
#   BENCH_WORKLOAD - ycsb_mix | ycsb_a | metakv | ibm
#   BENCH_DATASET_POW - dataset exponent for selected benchmark path
#   BENCH_EXP1_FOLDER - explicit scalability folder (e.g. YCSB_2^24)
#   CPU_AFFINITY   - taskset CPU list (e.g. 0 or 0,1)  (default: 0)
# ============================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR"
DOCKER_DIR="$SCRIPT_DIR/docker"
IMAGE_NAME="sapphmap:latest"
CONTAINER_NAME="sapphmap-dev"
APP_DIR="/workspace/applications/sapphmap"

# --- Defaults ---
RAM_SIZE="${RAM_SIZE:-128G}"
ENCLAVE_SIZE="${ENCLAVE_SIZE:-32GB}"
BLOCK_SIZE="${BLOCK_SIZE:-32KB}"
BUFFER_SIZE="${BUFFER_SIZE:-512KB}"
GAMMA="${GAMMA:-2}"
EPS_TOTAL="${EPS_TOTAL:-4.0}"
PAYLOAD="${PAYLOAD:-24}"
SGX_MODE="${SGX_MODE:-HW}"
SGX_PRERELEASE="${SGX_PRERELEASE:-1}"
SGX_DEBUG="${SGX_DEBUG:-0}"
DISK_IO="${DISK_IO:-1}"
DISK_IO_DATA_DIR="${DISK_IO_DATA_DIR:-${PROJECT_DIR}/.diskio}"
DISK_IO_STORAGE_FILE="${DISK_IO_STORAGE_FILE:-${DISK_IO_DATA_DIR%/}/storage.bin}"
IO_ROUND="${IO_ROUND:-1}"
JOBS="${JOBS:-$(nproc)}"
QUIET_MAKE="${QUIET_MAKE:-1}"
QUIET_DOCKER="${QUIET_DOCKER:-1}"
QUIET_SUMMARY="${QUIET_SUMMARY:-1}"
BENCH_TARGET="${BENCH_TARGET:-exp2_blocksize}"
BENCH_INDEX="${BENCH_INDEX:-sapphmap}"
BENCH_WORKLOAD="${BENCH_WORKLOAD:-ycsb_mix}"
BENCH_DATASET_POW="${BENCH_DATASET_POW:-24}"
BENCH_EXP1_FOLDER="${BENCH_EXP1_FOLDER:-}"
CPU_AFFINITY="${CPU_AFFINITY:-0}"
MAKE_LOG_PATH="${MAKE_LOG_PATH:-/tmp/sapphmap-makeoutput/sapphmap_make.log}"

# --- TTY detection ---
DOCKER_IT=""
if [ -t 0 ]; then
    DOCKER_IT="-it"
fi

# --- Docker run flags ---
docker_run_flags() {
    local flags=""

    # 1. Use --init so Ctrl+C can terminate container processes cleanly.
    # 2. Keep interactive TTY behavior when stdin is a terminal.
    flags+=" --rm ${DOCKER_IT} --init --name ${CONTAINER_NAME}-$(date +%s%N | tail -c 6)"

    # 3. Inject key environment variables.
    # Ensure the app and AESM can resolve required shared libraries.
    flags+=" -e SGX_MODE=${SGX_MODE}"
    flags+=" -e DISK_IO_STORAGE_FILE=${DISK_IO_STORAGE_FILE}"
    flags+=" -e LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu:/opt/intel/sgx-aesm-service/aesm/:/opt/conda/envs/sapphmap/lib"

    # Memory limit (Docker cgroup)
    if [ -n "$RAM_SIZE" ]; then
        flags+=" --memory=${RAM_SIZE} --memory-swap=${RAM_SIZE}"
    fi

    # Volume mount
    flags+=" -v ${PROJECT_DIR}:/workspace"
    if [ "${DISK_IO}" = "1" ]; then
        # Bind host storage target to app-local relative filename "storage.bin".
        flags+=" -v ${DISK_IO_STORAGE_FILE}:${APP_DIR}/storage.bin"
    elif [ -d "/home/data" ]; then
        # Keep a broad mount fallback for non-disk-io debugging workflows.
        flags+=" -v /home/data:/home/data"
    fi

    # Allow realtime scheduling for chrt -f 99.
    flags+=" --cap-add=SYS_NICE"

    # Mount SGX device nodes in HW mode.
    if [ "$SGX_MODE" = "HW" ]; then
        # Mount any SGX node available on the host.
        for dev in /dev/sgx_enclave /dev/sgx_provision /dev/isgx /dev/sgx/enclave /dev/sgx/provision; do
            if [ -e "$dev" ]; then
                flags+=" --device $dev"
            fi
        done
    fi

    echo "$flags"
}

ensure_disk_io_data_dir() {
    if [ "${DISK_IO}" = "1" ]; then
        local storage_dir
        storage_dir="$(dirname "${DISK_IO_STORAGE_FILE}")"
        mkdir -p "${storage_dir}"
        # Only create when missing; touching an existing root-owned file can fail.
        if [ ! -e "${DISK_IO_STORAGE_FILE}" ]; then
            touch "${DISK_IO_STORAGE_FILE}"
        fi
    fi
}

# --- Make arguments ---
make_args() {
    echo "SGX_MODE=${SGX_MODE} SGX_PRERELEASE=${SGX_PRERELEASE} SGX_DEBUG=${SGX_DEBUG} \
ENCLAVE_SIZE=${ENCLAVE_SIZE} BLOCK_SIZE=${BLOCK_SIZE} \
BUFFER_SIZE=${BUFFER_SIZE} DISK_IO=${DISK_IO} IO_ROUND=${IO_ROUND} \
GAMMA=${GAMMA} EPS_TOTAL=${EPS_TOTAL} PAYLOAD=${PAYLOAD} \
DISK_IO_STORAGE_FILE=${DISK_IO_STORAGE_FILE} \
BENCH_TARGET=${BENCH_TARGET} BENCH_INDEX=${BENCH_INDEX} BENCH_WORKLOAD=${BENCH_WORKLOAD} \
BENCH_DATASET_POW=${BENCH_DATASET_POW} BENCH_EXP1_FOLDER=${BENCH_EXP1_FOLDER} \
-j${JOBS}"
}

make_quiet_args() {
    if [ "${QUIET_MAKE}" = "1" ]; then
        echo "-s --no-print-directory"
    fi
}

bench_workload_display_name() {
    case "${BENCH_TARGET}" in
        exp1_workload|exp2_tradeoff)
            echo "${BENCH_WORKLOAD}"
            ;;
        exp1_scalability|exp1_rangequery|exp2_privacy|exp2_fanout|exp2_buffersize|exp2_blocksize|exp2_payload)
            echo "ycsb_fixsize"
            ;;
        *)
            echo "${BENCH_WORKLOAD}"
            ;;
    esac
}

docker_run_maybe_quiet() {
    if [ "${QUIET_DOCKER}" = "1" ]; then
        set -o pipefail
        docker run "$@" 2>&1 | sed -E \
            -e '/swap limit capabilities/d' \
            -e '/^\[Entrypoint\] SGX Hardware mode detected\. Starting AESM service\.\.\./d' \
            -e '/^aesm_service: warning: Turn to daemon\./d'
        local rc=${PIPESTATUS[0]}
        set +o pipefail
        return ${rc}
    fi
    docker run "$@"
}

# --- Commands ---
cmd_build() {
    echo "=== Building Docker image: ${IMAGE_NAME} ==="
    docker build -t "${IMAGE_NAME}" -f "${DOCKER_DIR}/Dockerfile" "${DOCKER_DIR}"
}

cmd_all() {
    ensure_disk_io_data_dir
    local bench_workload_display
    bench_workload_display="$(bench_workload_display_name)"

    echo "[RUN] ENCLAVE_SIZE=${ENCLAVE_SIZE} RAM_SIZE=${RAM_SIZE:-None}"
    echo "[RUN] BENCH_TARGET=${BENCH_TARGET} BENCH_INDEX=${BENCH_INDEX} BENCH_DATASET_POW=${BENCH_DATASET_POW}"
    [ -n "${bench_workload_display}" ] && echo "[RUN] BENCH_WORKLOAD=${bench_workload_display}"
    [ -n "${BENCH_EXP1_FOLDER}" ] && echo "[RUN] BENCH_EXP1_FOLDER=${BENCH_EXP1_FOLDER}"
    if [ "${QUIET_SUMMARY}" != "1" ]; then
        echo "=== Cleaning, Compiling, and Running SapphMap ==="
        echo "  SGX_MODE=${SGX_MODE}  ENCLAVE_SIZE=${ENCLAVE_SIZE}  RAM_LIMIT=${RAM_SIZE:-None}"
        echo "  BLOCK_SIZE=${BLOCK_SIZE}  BUFFER_SIZE=${BUFFER_SIZE}"
        echo "  GAMMA=${GAMMA}  EPS_TOTAL=${EPS_TOTAL}"
        echo "  DISK_IO=${DISK_IO}  SGX_DEBUG=${SGX_DEBUG}  JOBS=${JOBS}"
        echo "  QUIET_MAKE=${QUIET_MAKE}"
        echo "  QUIET_DOCKER=${QUIET_DOCKER}"
    fi
    
    # Run everything in a single container lifespan
    local _run_rc=0
    if [ "${QUIET_MAKE}" = "1" ]; then
        if [ "${QUIET_SUMMARY}" != "1" ]; then
            echo "  Build log path=${MAKE_LOG_PATH} (hidden unless build fails)"
        fi
        docker_run_maybe_quiet $(docker_run_flags) -w "${APP_DIR}" "${IMAGE_NAME}" \
            bash -c "set -e; LOG_PATH=\"${MAKE_LOG_PATH}\"; LOG_DIR=\$(dirname \"\$LOG_PATH\"); mkdir -p \"\$LOG_DIR\"; make -s --no-print-directory clean >/dev/null 2>&1; make $(make_quiet_args) $(make_args) >\"\$LOG_PATH\" 2>&1 || { echo \"[ERROR] build failed, showing \$LOG_PATH\"; if [ -f \"\$LOG_PATH\" ]; then cat \"\$LOG_PATH\"; else echo \"[ERROR] log file not found: \$LOG_PATH\"; fi; exit 1; }; exec chrt -f 99 taskset -c ${CPU_AFFINITY} ./sapphmap.elf" \
            || _run_rc=$?
    else
        docker_run_maybe_quiet $(docker_run_flags) -w "${APP_DIR}" "${IMAGE_NAME}" \
            bash -c "set -e; make -s --no-print-directory clean >/dev/null 2>&1; make $(make_quiet_args) $(make_args); exec chrt -f 99 taskset -c ${CPU_AFFINITY} ./sapphmap.elf" \
            || _run_rc=$?
    fi
    # Exactly one blank line before the next ./run.sh [RUN] block (success or failure).
    printf '\n'
    return "${_run_rc}"
}

cmd_make() {
    ensure_disk_io_data_dir
    local extra_args="$*"
    echo "=== Compiling sapphmap (Make only) ==="
    echo "  QUIET_MAKE=${QUIET_MAKE}"
    docker_run_maybe_quiet $(docker_run_flags) -w "${APP_DIR}" "${IMAGE_NAME}" \
        make $(make_quiet_args) $(make_args) ${extra_args}
}

cmd_run() {
    ensure_disk_io_data_dir
    local run_cmd="${*:-chrt -f 99 taskset -c ${CPU_AFFINITY} ./sapphmap.elf}"
    echo "=== Running: ${run_cmd} ==="
    docker_run_maybe_quiet $(docker_run_flags) -w "${APP_DIR}" "${IMAGE_NAME}" ${run_cmd}
}

cmd_exec() {
    ensure_disk_io_data_dir
    docker_run_maybe_quiet $(docker_run_flags) -w "${APP_DIR}" "${IMAGE_NAME}" "$@"
}

cmd_shell() {
    ensure_disk_io_data_dir
    echo "=== Interactive shell ==="
    docker run $(docker_run_flags) -w "${APP_DIR}" "${IMAGE_NAME}" bash
}

cmd_clean() {
    echo "=== Cleaning build artifacts ==="
    docker_run_maybe_quiet $(docker_run_flags) -w "${APP_DIR}" "${IMAGE_NAME}" make clean
}

# --- Main ---
# Default to "all" if no arguments provided
CMD="${1:-all}"

case "$CMD" in
    all)
        cmd_all ;;
    build)
        shift; cmd_build "$@" ;;
    make)
        shift; cmd_make "$@" ;;
    run)
        shift; cmd_run "$@" ;;
    exec)
        shift; cmd_exec "$@" ;;
    shell)
        shift; cmd_shell "$@" ;;
    clean)
        shift; cmd_clean "$@" ;;
    help|--help|-h)
        head -22 "$0" | grep '^#' | sed 's/^# \?//'
        echo ""
        echo "Examples:"
        echo "  ./run.sh                  # One-click clean, compile, and run"
        echo "  SGX_MODE=HW ./run.sh      # Same as above, but in Hardware Mode"
        echo "  RAM_SIZE=4g ./run.sh      # Run with 4GB memory limit"
        echo "  ./run.sh shell            # Enter container for debugging"
        ;;
    *)
        echo "Unknown command: $1 (try: all|build|make|run|shell|clean|help)"
        exit 1 ;;
esac