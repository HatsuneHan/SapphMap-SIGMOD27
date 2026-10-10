#!/usr/bin/env bash
set -euo pipefail

# ============================================================
# Unified experiment orchestrator aligned with paper narrative.
#
# Exp1 (core comparison):
#   ./run_exp.sh Exp1 scalability <18|19|...|all> <sapphmap|enigmap|nolsmtree|nohashmap|h2o2ram>
#   ./run_exp.sh Exp1 workload <metakv|ibm|ycsb_mix|ycsb_a|all> <sapphmap|enigmap|nolsmtree|nohashmap|h2o2ram>
#   ./run_exp.sh Exp1 rangequery all <sapphmap|enigmap|nolsmtree|nohashmap|h2o2ram>
#
# Exp2 (parameter sensitivity, Sapphmap only):
#   ./run_exp.sh Exp2 blocksize
#   ./run_exp.sh Exp2 buffersize
#   ./run_exp.sh Exp2 fanout
#   ./run_exp.sh Exp2 privacy
#   ./run_exp.sh Exp2 payload
#   ./run_exp.sh Exp2 tradeoff
#
# Notes:
# - This script only reorganizes experiment entrypoints; run.sh remains the
#   single build/run backend with the original parameter passing mechanism.
# - Rangequery is fixed to YCSB_2^24 and executes the full internal
#   selectivity sweep (all selectivities are handled in enclave code).
# ============================================================

RAM_SIZE="${RAM_SIZE:-}"
ENCLAVE_SIZE="${ENCLAVE_SIZE:-}"
BLOCK_SIZE="${BLOCK_SIZE:-32KB}"
BUFFER_SIZE="${BUFFER_SIZE:-512KB}"
GAMMA="${GAMMA:-2}"
EPS_TOTAL="${EPS_TOTAL:-4.0}"
PAYLOAD="${PAYLOAD:-24}"
BENCH_DATASET_POW="${BENCH_DATASET_POW:-24}"
SGX_MODE="${SGX_MODE:-HW}"
DISK_IO_DATA_DIR="${DISK_IO_DATA_DIR:-/home/data/sapphmap}"
DISK_IO_STORAGE_FILE="${DISK_IO_STORAGE_FILE:-${DISK_IO_DATA_DIR%/}/storage.bin}"
CPU_AFFINITY="${CPU_AFFINITY:-18}"

# run_exp.sh fixes quiet mode internally; quiet knobs stay in run.sh only.
QUIET_MAKE="1"
QUIET_DOCKER="1"
QUIET_SUMMARY="1"

PROFILE_COMPARISON_RAM="128G"
PROFILE_COMPARISON_ENCLAVE="32GB"
PROFILE_PARAMETER_RAM="1G"
PROFILE_PARAMETER_ENCLAVE="256MB"

print_sep() {
    echo "============================================================"
}

use_comparison_profile_defaults() {
    RAM_SIZE="${PROFILE_COMPARISON_RAM}"
    ENCLAVE_SIZE="${PROFILE_COMPARISON_ENCLAVE}"
}

use_parameter_profile_defaults() {
    RAM_SIZE="${PROFILE_PARAMETER_RAM}"
    ENCLAVE_SIZE="${PROFILE_PARAMETER_ENCLAVE}"
}

validate_index() {
    local index="$1"
    case "$index" in
        sapphmap|nolsmtree|enigmap|nohashmap|h2o2ram) ;;
        *)
            echo "[ERROR] invalid index: ${index} (expected sapphmap|nolsmtree|enigmap|nohashmap|h2o2ram)" >&2
            return 1
            ;;
    esac
}

normalize_workload() {
    local key="$1"
    case "$key" in
        ycsb_a|ycsba|a)
            echo "ycsb_a"
            ;;
        ycsb_mix|mix)
            echo "ycsb_mix"
            ;;
        metakv|meta)
            echo "metakv"
            ;;
        ibm)
            echo "ibm"
            ;;
        *)
            echo "[ERROR] invalid workload: ${key}" >&2
            return 1
            ;;
    esac
}

run_with_env() {
    local bench_target="$1"
    local bench_index="$2"
    local bench_workload="$3"
    local bench_dataset_pow="$4"
    local bench_exp1_folder="${5:-}"

    RAM_SIZE="${RAM_SIZE}" \
        ENCLAVE_SIZE="${ENCLAVE_SIZE}" \
        BLOCK_SIZE="${BLOCK_SIZE}" \
        BUFFER_SIZE="${BUFFER_SIZE}" \
        GAMMA="${GAMMA}" \
        EPS_TOTAL="${EPS_TOTAL}" \
        PAYLOAD="${PAYLOAD}" \
        BENCH_TARGET="${bench_target}" \
        BENCH_INDEX="${bench_index}" \
        BENCH_WORKLOAD="${bench_workload}" \
        BENCH_DATASET_POW="${bench_dataset_pow}" \
        BENCH_EXP1_FOLDER="${bench_exp1_folder}" \
        SGX_MODE="${SGX_MODE}" \
        DISK_IO_DATA_DIR="${DISK_IO_DATA_DIR}" \
        DISK_IO_STORAGE_FILE="${DISK_IO_STORAGE_FILE}" \
        QUIET_MAKE="${QUIET_MAKE}" \
        QUIET_DOCKER="${QUIET_DOCKER}" \
        QUIET_SUMMARY="${QUIET_SUMMARY}" \
        CPU_AFFINITY="${CPU_AFFINITY}" \
        ./run.sh
}

run_exp1_scalability_one() {
    local power="$1"
    local index="$2"
    validate_index "${index}"
    use_comparison_profile_defaults

    local folder="YCSB_2^${power}"
    print_sep
    echo "[Exp1/scalability] profile=${RAM_SIZE}/${ENCLAVE_SIZE} data=${folder} index=${index}"
    print_sep
    run_with_env "exp1_scalability" "${index}" "ycsb_mix" "${power}" "${folder}"
}

run_exp1_scalability() {
    local selector="$1"
    local index="$2"

    if [ "${selector}" = "all" ]; then
        local p
        for p in $(seq 18 31); do
            run_exp1_scalability_one "${p}" "${index}"
        done
        return
    fi

    if ! [[ "${selector}" =~ ^[0-9]+$ ]]; then
        echo "[ERROR] scalability selector must be <power|all>, got: ${selector}" >&2
        return 1
    fi

    run_exp1_scalability_one "${selector}" "${index}"
}

run_exp1_workload_one() {
    local workload="$1"
    local index="$2"
    validate_index "${index}"
    use_comparison_profile_defaults

    print_sep
    echo "[Exp1/workload] profile=${RAM_SIZE}/${ENCLAVE_SIZE} workload=${workload} index=${index}"
    print_sep
    run_with_env "exp1_workload" "${index}" "${workload}" "${BENCH_DATASET_POW}"
}

run_exp1_workload() {
    local selector="$1"
    local index="$2"
    validate_index "${index}"

    if [ "${selector}" = "all" ]; then
        local workload
        for workload in ycsb_a ycsb_mix metakv ibm; do
            run_exp1_workload_one "${workload}" "${index}"
        done
        return
    fi

    run_exp1_workload_one "$(normalize_workload "${selector}")" "${index}"
}

run_exp1_rangequery_once() {
    local dataset_pow="$1"
    local index="$2"
    print_sep
    echo "[Exp1/rangequery] profile=${RAM_SIZE}/${ENCLAVE_SIZE} data=YCSB_2^${dataset_pow} index=${index} (full selectivity sweep)"
    print_sep
    run_with_env "exp1_rangequery" "${index}" "ycsb_mix" "${dataset_pow}"
}

run_exp1_rangequery() {
    local selector="$1"
    local index="$2"
    validate_index "${index}"

    if [ "${selector}" != "all" ]; then
        echo "[ERROR] rangequery selector must be 'all' (fixed dataset: YCSB_2^24), got: ${selector}" >&2
        return 1
    fi

    local dataset_pow="24"

    use_comparison_profile_defaults
    run_exp1_rangequery_once "${dataset_pow}" "${index}"

    use_parameter_profile_defaults
    run_exp1_rangequery_once "${dataset_pow}" "${index}"
}

run_exp2_privacy_once() {
    local profile_label="$1"
    local EPS_TOTAL
    print_sep
    echo "[Exp2/privacy] profile=${profile_label} sweep: EPS_TOTAL"
    print_sep
    for EPS_TOTAL in 1.0 2.0 4.0 8.0 16.0; do
        run_with_env "exp2_privacy" "sapphmap" "ycsb_mix" "${BENCH_DATASET_POW}"
    done
}

run_exp2_privacy() {
    use_comparison_profile_defaults
    run_exp2_privacy_once "${RAM_SIZE}/${ENCLAVE_SIZE}"

    use_parameter_profile_defaults
    run_exp2_privacy_once "${RAM_SIZE}/${ENCLAVE_SIZE}"
}

run_exp2_fanout_once() {
    local profile_label="$1"
    local GAMMA
    print_sep
    echo "[Exp2/fanout] profile=${profile_label} sweep: GAMMA"
    print_sep
    for GAMMA in 2 4 6 8 10 12; do
        run_with_env "exp2_fanout" "sapphmap" "ycsb_mix" "${BENCH_DATASET_POW}"
    done
}

run_exp2_fanout() {
    use_comparison_profile_defaults
    run_exp2_fanout_once "${RAM_SIZE}/${ENCLAVE_SIZE}"

    use_parameter_profile_defaults
    run_exp2_fanout_once "${RAM_SIZE}/${ENCLAVE_SIZE}"
}

run_exp2_buffersize_once() {
    local profile_label="$1"
    local BUFFER_SIZE
    print_sep
    echo "[Exp2/buffersize] profile=${profile_label} sweep: BUFFER_SIZE"
    print_sep
    for BUFFER_SIZE in 64KB 32KB 128KB 256KB 512KB 1MB 2MB; do
        run_with_env "exp2_buffersize" "sapphmap" "ycsb_mix" "${BENCH_DATASET_POW}"
    done
}

run_exp2_buffersize() {
    use_comparison_profile_defaults
    run_exp2_buffersize_once "${RAM_SIZE}/${ENCLAVE_SIZE}"

    use_parameter_profile_defaults
    run_exp2_buffersize_once "${RAM_SIZE}/${ENCLAVE_SIZE}"
}

run_exp2_blocksize() {
    use_parameter_profile_defaults
    local dataset_pow BLOCK_SIZE rep
    print_sep
    echo "[Exp2/blocksize] profile=${RAM_SIZE}/${ENCLAVE_SIZE} sweep: 2^19..2^26 x block sizes"
    print_sep
    for dataset_pow in 19 20 21 22 23 24 25 26; do
        for BLOCK_SIZE in 4KB 8KB 16KB 32KB 64KB; do
            run_with_env "exp2_blocksize" "sapphmap" "ycsb_mix" "${dataset_pow}"
        done
    done
}

run_exp2_payload_once() {
    local profile_label="$1"
    local PAYLOAD BLOCK_SIZE
    BLOCK_SIZE="32KB"  # Fixed for divisibility
    print_sep
    echo "[Exp2/payload] profile=${profile_label} block=${BLOCK_SIZE} sweep: PAYLOAD"
    print_sep
    for PAYLOAD in 24 40 56 72 88; do
        run_with_env "exp2_payload" "sapphmap" "ycsb_mix" "${BENCH_DATASET_POW}"
    done
}

run_exp2_payload() {
    use_comparison_profile_defaults
    run_exp2_payload_once "${RAM_SIZE}/${ENCLAVE_SIZE}"

    use_parameter_profile_defaults
    run_exp2_payload_once "${RAM_SIZE}/${ENCLAVE_SIZE}"
}

run_exp2_tradeoff_once() {
    local profile_label="$1"
    local workload="$2"
    local BUFFER_SIZE
    print_sep
    echo "[Exp2/tradeoff] profile=${profile_label} workload=${workload} sweep: BUFFER_SIZE"
    print_sep
    for BUFFER_SIZE in 64KB 32KB 128KB 256KB 512KB 1MB 2MB; do
        run_with_env "exp2_tradeoff" "sapphmap" "${workload}" "${BENCH_DATASET_POW}"
    done
}

run_exp2_tradeoff() {
    local workloads="ycsb_a ycsb_mix"
    local wl

    use_comparison_profile_defaults
    for wl in ${workloads}; do
        run_exp2_tradeoff_once "${RAM_SIZE}/${ENCLAVE_SIZE}" "${wl}"
    done

    use_parameter_profile_defaults
    for wl in ${workloads}; do
        run_exp2_tradeoff_once "${RAM_SIZE}/${ENCLAVE_SIZE}" "${wl}"
    done
}

usage() {
    cat <<'EOF'
Usage:
    ./run_exp.sh [--disk-io-data-dir /path/to/data-dir] Exp1 scalability <18|19|...|all> <sapphmap|enigmap|nolsmtree|nohashmap|h2o2ram>
    ./run_exp.sh [--disk-io-data-dir /path/to/data-dir] Exp1 workload <metakv|ibm|ycsb_mix|ycsb_a|all> <sapphmap|enigmap|nolsmtree|nohashmap|h2o2ram>
    ./run_exp.sh [--disk-io-data-dir /path/to/data-dir] Exp1 rangequery all <sapphmap|enigmap|nolsmtree|nohashmap|h2o2ram>

    ./run_exp.sh [--disk-io-data-dir /path/to/data-dir] Exp2 blocksize
    ./run_exp.sh [--disk-io-data-dir /path/to/data-dir] Exp2 buffersize
    ./run_exp.sh [--disk-io-data-dir /path/to/data-dir] Exp2 fanout
    ./run_exp.sh [--disk-io-data-dir /path/to/data-dir] Exp2 privacy
    ./run_exp.sh [--disk-io-data-dir /path/to/data-dir] Exp2 payload
    ./run_exp.sh [--disk-io-data-dir /path/to/data-dir] Exp2 tradeoff

Options:
    --disk-io-data-dir <dir>
        Directory path only. run_exp.sh will append '/storage.bin' and forward
        DISK_IO_STORAGE_FILE to run.sh -> make.

Trailing positional data-dir is also supported:
    ./run_exp.sh Exp1 scalability all sapphmap /path/to/data-dir
    ./run_exp.sh Exp2 blocksize /path/to/data-dir

Legacy shorthand (without option) still works:
    ./run_exp.sh Exp1 scalability <18|19|...|all> <sapphmap|enigmap|nolsmtree|nohashmap|h2o2ram>
    ./run_exp.sh Exp1 workload <metakv|ibm|ycsb_mix|ycsb_a|all> <sapphmap|enigmap|nolsmtree|nohashmap|h2o2ram>
    ./run_exp.sh Exp1 rangequery all <sapphmap|enigmap|nolsmtree|nohashmap|h2o2ram>

  ./run_exp.sh Exp2 blocksize
  ./run_exp.sh Exp2 buffersize
  ./run_exp.sh Exp2 fanout
  ./run_exp.sh Exp2 privacy
  ./run_exp.sh Exp2 payload
  ./run_exp.sh Exp2 tradeoff
EOF
}

main() {
    local disk_io_data_dir_from_cli=0

    while [ "$#" -gt 0 ]; do
        case "$1" in
            --disk-io-data-dir)
                if [ "$#" -lt 2 ]; then
                    echo "[ERROR] --disk-io-data-dir requires a directory argument." >&2
                    usage
                    exit 1
                fi
                DISK_IO_DATA_DIR="$2"
                disk_io_data_dir_from_cli=1
                shift 2
                ;;
            --help|-h)
                usage
                exit 0
                ;;
            --)
                shift
                break
                ;;
            *)
                break
                ;;
        esac
    done

    if [ "$#" -lt 2 ]; then
        usage
        exit 1
    fi

    # [ "${disk_io_data_dir_from_cli}" -eq 1 ] || { echo "[ERROR] --disk-io-data-dir is required." >&2; exit 1; }

    local exp_group="$1"
    local exp_case="$2"

    case "${exp_group}" in
        Exp1|exp1)
            if [ "$#" -lt 4 ] || [ "$#" -gt 5 ]; then
                usage
                exit 1
            fi
            local selector="$3"
            local index="$4"
            if [ "$#" -eq 5 ]; then
                DISK_IO_DATA_DIR="$5"
                disk_io_data_dir_from_cli=1
            fi
            DISK_IO_STORAGE_FILE="${DISK_IO_DATA_DIR%/}/storage.bin"
            case "${exp_case}" in
                scalability)
                    run_exp1_scalability "${selector}" "${index}"
                    ;;
                workload)
                    run_exp1_workload "${selector}" "${index}"
                    ;;
                rangequery)
                    run_exp1_rangequery "${selector}" "${index}"
                    ;;
                *)
                    echo "[ERROR] invalid Exp1 case: ${exp_case}" >&2
                    usage
                    exit 1
                    ;;
            esac
            ;;
        Exp2|exp2)
            if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
                echo "[ERROR] Exp2 accepts at most one optional trailing data-dir argument." >&2
                usage
                exit 1
            fi
            if [ "$#" -eq 3 ]; then
                DISK_IO_DATA_DIR="$3"
                disk_io_data_dir_from_cli=1
            fi
            DISK_IO_STORAGE_FILE="${DISK_IO_DATA_DIR%/}/storage.bin"
            case "${exp_case}" in
                blocksize) run_exp2_blocksize ;;
                buffersize) run_exp2_buffersize ;;
                fanout) run_exp2_fanout ;;
                privacy) run_exp2_privacy ;;
                payload) run_exp2_payload ;;
                tradeoff) run_exp2_tradeoff ;;
                *)
                    echo "[ERROR] invalid Exp2 case: ${exp_case}" >&2
                    usage
                    exit 1
                    ;;
            esac
            ;;
        *)
            echo "[ERROR] invalid experiment group: ${exp_group}" >&2
            usage
            exit 1
            ;;
    esac
}

main "$@"
